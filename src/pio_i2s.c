#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <opus.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/flash.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>

#define I2S_NODE DT_NODELABEL(pio1_i2s)

#define I2S_DEFAULT_SAMPLE_RATE 48000U
#define I2S_SLOT_BITS 32U
#define I2S_CHANNELS 2U
#define I2S_THREAD_STACK_SIZE 12288
#define I2S_DMA_WORDS 512U
#define I2S_USE_RX_DMA 1
#define I2S_DEBUG_HEAD_WORDS 4U
#define I2S_RECORD_MAX_SAMPLES 16000U
#define I2S_RECORD_DUMP_SAMPLES_PER_LINE 32U
#define I2S_FLASH_RECORD_OFFSET 0x300000U
#define I2S_FLASH_RECORD_SIZE 0x80000U
#define I2S_FLASH_RECORD_CHUNK_SAMPLES 256U
#define I2S_OPUS_RECORD_OFFSET 0x380000U
#define I2S_OPUS_RECORD_SIZE 0x70000U
#define I2S_OPUS_FRAME_MS 20U
#define I2S_OPUS_FRAME_SAMPLES 320U
#define I2S_OPUS_PACKET_MAX 400U
#define I2S_OPUS_FLASH_PAGE 256U
#define I2S_OPUS_ENCODER_BYTES 30000U
#define I2S_OPUS_DEFAULT_BITRATE 24000
#define I2S_OPUS_MAGIC 0x3153504fU /* OPS1 */

PINCTRL_DT_DEFINE(I2S_NODE);

static const struct device *const i2s_pio_dev = DEVICE_DT_GET(DT_PARENT(I2S_NODE));
static const struct pinctrl_dev_config *const i2s_pinctrl = PINCTRL_DT_DEV_CONFIG_GET(I2S_NODE);

static K_THREAD_STACK_DEFINE(i2s_thread_stack, I2S_THREAD_STACK_SIZE);
static struct k_thread i2s_thread_data;
static k_tid_t i2s_thread_id;

static PIO i2s_pio;
static size_t i2s_clk_sm;
static size_t i2s_rx_sm;
static uint i2s_clk_offset;
static uint i2s_rx_offset;
static int i2s_rx_dma_chan = -1;
static int i2s_rx_dma_ctrl_chan = -1;
static bool i2s_initialized;
static volatile bool i2s_running;
static bool i2s_tx_enabled = true;
static uint32_t i2s_sample_rate = I2S_DEFAULT_SAMPLE_RATE;
static uint32_t i2s_bclk_pin;
static uint32_t i2s_lrclk_pin;
static uint32_t i2s_data_pin;
static uint32_t i2s_dout_pin;
static uint32_t i2s_rx_dma_buffer[2][I2S_DMA_WORDS] __aligned(8);
static uint32_t *i2s_rx_dma_ctrl_blocks[2] __aligned(8);
static volatile uint32_t i2s_rx_words_seen;
static volatile uint32_t i2s_rx_last_word;
static int16_t i2s_record_buffer[I2S_RECORD_MAX_SAMPLES];
static volatile bool i2s_recording;
static volatile uint32_t i2s_record_target_samples;
static volatile uint32_t i2s_record_samples;
static uint32_t i2s_record_rate;
static volatile bool i2s_dumping;
static volatile bool i2s_flash_recording;
static volatile bool i2s_flash_record_done;
static volatile bool i2s_flash_record_error;
static uint32_t i2s_flash_record_target_samples;
static uint32_t i2s_flash_record_samples;
static uint32_t i2s_flash_record_offset;
static uint32_t i2s_flash_record_rate;
static uint32_t i2s_flash_chunk_samples;
static int16_t i2s_flash_chunk[I2S_FLASH_RECORD_CHUNK_SAMPLES];
static volatile bool i2s_opus_recording;
static volatile bool i2s_opus_record_done;
static volatile bool i2s_opus_record_error;
static uint32_t i2s_opus_record_target_samples;
static uint32_t i2s_opus_record_samples;
static uint32_t i2s_opus_record_rate;
static uint32_t i2s_opus_record_frames;
static uint32_t i2s_opus_record_offset;
static uint32_t i2s_opus_record_bytes;
static int i2s_opus_bitrate = I2S_OPUS_DEFAULT_BITRATE;
static int i2s_opus_last_error;
static uint32_t i2s_opus_frame_samples;
static int16_t i2s_opus_frame[I2S_OPUS_FRAME_SAMPLES];
static uint8_t i2s_opus_packet[I2S_OPUS_PACKET_MAX];
static uint8_t i2s_opus_flash_page[I2S_OPUS_FLASH_PAGE];
static uint32_t i2s_opus_flash_page_used;
static uint8_t i2s_opus_encoder_storage[I2S_OPUS_ENCODER_BYTES] __aligned(8);
static OpusEncoder *i2s_opus_encoder = (OpusEncoder *)i2s_opus_encoder_storage;

static const int16_t tone_table[] = {
	0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
	23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
	32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
	23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
	0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
	-23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
	-32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
	-23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212,
};

static uint16_t i2s_clk_instructions[6];
static uint16_t i2s_rx_instructions[24];

static struct pio_program i2s_clk_program = {
	.instructions = i2s_clk_instructions,
	.length = ARRAY_SIZE(i2s_clk_instructions),
	.origin = -1,
};

static struct pio_program i2s_rx_program = {
	.instructions = i2s_rx_instructions,
	.length = ARRAY_SIZE(i2s_rx_instructions),
	.origin = -1,
};

static void i2s_programs_build(uint bclk_pin, uint lrclk_pin)
{
	i2s_clk_instructions[0] = pio_encode_set(pio_x, I2S_SLOT_BITS - 1) | pio_encode_sideset(2, 0);
	i2s_clk_instructions[1] = pio_encode_nop() | pio_encode_sideset(2, 1);
	i2s_clk_instructions[2] = pio_encode_jmp_x_dec(1) | pio_encode_sideset(2, 0);
	i2s_clk_instructions[3] = pio_encode_set(pio_x, I2S_SLOT_BITS - 1) | pio_encode_sideset(2, 2);
	i2s_clk_instructions[4] = pio_encode_nop() | pio_encode_sideset(2, 3);
	i2s_clk_instructions[5] = pio_encode_jmp_x_dec(4) | pio_encode_sideset(2, 2);

	i2s_rx_instructions[0] = pio_encode_wait_gpio(true, lrclk_pin);
	i2s_rx_instructions[1] = pio_encode_wait_gpio(false, lrclk_pin);
	i2s_rx_instructions[2] = pio_encode_wait_gpio(true, bclk_pin);
	i2s_rx_instructions[3] = pio_encode_wait_gpio(false, bclk_pin);
	i2s_rx_instructions[4] = pio_encode_set(pio_x, 23);
	i2s_rx_instructions[5] = pio_encode_wait_gpio(true, bclk_pin);
	i2s_rx_instructions[6] = pio_encode_nop();
	i2s_rx_instructions[7] = pio_encode_nop();
	i2s_rx_instructions[8] = pio_encode_in(pio_pins, 1);
	i2s_rx_instructions[9] = pio_encode_wait_gpio(false, bclk_pin);
	i2s_rx_instructions[10] = pio_encode_jmp_x_dec(5);
	i2s_rx_instructions[11] = pio_encode_in(pio_null, 8);
	i2s_rx_instructions[12] = pio_encode_wait_gpio(true, lrclk_pin);
	i2s_rx_instructions[13] = pio_encode_wait_gpio(true, bclk_pin);
	i2s_rx_instructions[14] = pio_encode_wait_gpio(false, bclk_pin);
	i2s_rx_instructions[15] = pio_encode_set(pio_x, 23);
	i2s_rx_instructions[16] = pio_encode_wait_gpio(true, bclk_pin);
	i2s_rx_instructions[17] = pio_encode_nop();
	i2s_rx_instructions[18] = pio_encode_nop();
	i2s_rx_instructions[19] = pio_encode_in(pio_pins, 1);
	i2s_rx_instructions[20] = pio_encode_wait_gpio(false, bclk_pin);
	i2s_rx_instructions[21] = pio_encode_jmp_x_dec(16);
	i2s_rx_instructions[22] = pio_encode_in(pio_null, 8);
	i2s_rx_instructions[23] = pio_encode_jmp(0);
}

static float i2s_clock_div(uint32_t sample_rate)
{
	uint32_t bit_clock = sample_rate * I2S_CHANNELS * I2S_SLOT_BITS;

	return (float)clock_get_hz(clk_sys) / (float)(bit_clock * 2U);
}

static float i2s_rx_clock_div(uint32_t sample_rate)
{
	uint32_t rx_pio_clock = sample_rate * I2S_CHANNELS * I2S_SLOT_BITS * 32U;

	return (float)clock_get_hz(clk_sys) / (float)rx_pio_clock;
}

static uint32_t i2s_tone_word(uint32_t sample_index, uint32_t sample_rate)
{
	uint32_t step = (ARRAY_SIZE(tone_table) * 440U);
	uint32_t index = ((uint64_t)sample_index * step / sample_rate) % ARRAY_SIZE(tone_table);

	return (uint32_t)((int32_t)tone_table[index] << 16);
}

static int16_t i2s_pcm16_from_raw(uint32_t raw)
{
	return (int16_t)(raw >> 16);
}

static void i2s_flash_program_page(uint32_t offset, const uint8_t *data)
{
	unsigned int key = irq_lock();

	flash_range_program(offset, data, I2S_OPUS_FLASH_PAGE);
	irq_unlock(key);
}

static void i2s_record_sample(int16_t sample)
{
	uint32_t pos;

	if (!i2s_recording) {
		return;
	}

	pos = i2s_record_samples;
	if (pos >= i2s_record_target_samples || pos >= ARRAY_SIZE(i2s_record_buffer)) {
		i2s_recording = false;
		return;
	}

	i2s_record_buffer[pos] = sample;
	pos++;
	i2s_record_samples = pos;
	if (pos >= i2s_record_target_samples) {
		i2s_recording = false;
	}
}

static void i2s_record_frame(int32_t left, int32_t right)
{
	int32_t left_abs = left < 0 ? -left : left;
	int32_t right_abs = right < 0 ? -right : right;

	i2s_record_sample((int16_t)(right_abs > left_abs ? right : left));
}

static void i2s_opus_flash_append(const uint8_t *data, uint32_t len)
{
	while (len > 0U && !i2s_opus_record_error) {
		uint32_t n = MIN(len, I2S_OPUS_FLASH_PAGE - i2s_opus_flash_page_used);

		memcpy(&i2s_opus_flash_page[i2s_opus_flash_page_used], data, n);
		i2s_opus_flash_page_used += n;
		data += n;
		len -= n;

		if (i2s_opus_flash_page_used == I2S_OPUS_FLASH_PAGE) {
			if (i2s_opus_record_offset + I2S_OPUS_FLASH_PAGE >
			    I2S_OPUS_RECORD_OFFSET + I2S_OPUS_RECORD_SIZE) {
				i2s_opus_record_error = true;
				i2s_opus_recording = false;
				i2s_opus_last_error = -ENOSPC;
				return;
			}
			i2s_flash_program_page(i2s_opus_record_offset, i2s_opus_flash_page);
			i2s_opus_record_offset += I2S_OPUS_FLASH_PAGE;
			i2s_opus_record_bytes += I2S_OPUS_FLASH_PAGE;
			i2s_opus_flash_page_used = 0U;
			memset(i2s_opus_flash_page, 0xff, sizeof(i2s_opus_flash_page));
		}
	}
}

static void i2s_opus_flash_flush_final(void)
{
	if (i2s_opus_flash_page_used == 0U || i2s_opus_record_error) {
		return;
	}
	if (i2s_opus_record_offset + I2S_OPUS_FLASH_PAGE >
	    I2S_OPUS_RECORD_OFFSET + I2S_OPUS_RECORD_SIZE) {
		i2s_opus_record_error = true;
		i2s_opus_last_error = -ENOSPC;
		return;
	}
	i2s_flash_program_page(i2s_opus_record_offset, i2s_opus_flash_page);
	i2s_opus_record_offset += I2S_OPUS_FLASH_PAGE;
	i2s_opus_record_bytes += I2S_OPUS_FLASH_PAGE;
	i2s_opus_flash_page_used = 0U;
}

static void i2s_opus_record_finish(void)
{
	uint8_t end_marker[2] = {0, 0};

	i2s_opus_flash_append(end_marker, sizeof(end_marker));
	i2s_opus_flash_flush_final();
	i2s_opus_recording = false;
	i2s_opus_record_done = !i2s_opus_record_error;
}

static void i2s_opus_encode_frame(void)
{
	uint8_t len_le[2];
	int ret;

	ret = opus_encode(i2s_opus_encoder, i2s_opus_frame,
			  I2S_OPUS_FRAME_SAMPLES, i2s_opus_packet,
			  sizeof(i2s_opus_packet));
	if (ret < 0) {
		i2s_opus_record_error = true;
		i2s_opus_recording = false;
		i2s_opus_last_error = ret;
		return;
	}

	len_le[0] = (uint8_t)(ret & 0xff);
	len_le[1] = (uint8_t)((uint32_t)ret >> 8);
	i2s_opus_flash_append(len_le, sizeof(len_le));
	i2s_opus_flash_append(i2s_opus_packet, (uint32_t)ret);
	i2s_opus_record_frames++;
}

static void i2s_opus_record_sample(int16_t sample)
{
	if (!i2s_opus_recording || i2s_opus_record_error) {
		return;
	}

	if (i2s_opus_record_samples >= i2s_opus_record_target_samples) {
		i2s_opus_record_finish();
		return;
	}

	i2s_opus_frame[i2s_opus_frame_samples++] = sample;
	i2s_opus_record_samples++;
	if (i2s_opus_frame_samples >= I2S_OPUS_FRAME_SAMPLES) {
		i2s_opus_encode_frame();
		i2s_opus_frame_samples = 0U;
	}

	if (i2s_opus_record_samples >= i2s_opus_record_target_samples) {
		if (i2s_opus_frame_samples > 0U) {
			memset(&i2s_opus_frame[i2s_opus_frame_samples], 0,
			       (I2S_OPUS_FRAME_SAMPLES - i2s_opus_frame_samples) *
			       sizeof(i2s_opus_frame[0]));
			i2s_opus_encode_frame();
			i2s_opus_frame_samples = 0U;
		}
		i2s_opus_record_finish();
	}
}

static void i2s_flash_record_flush(void)
{
	size_t bytes;
	unsigned int key;

	if (i2s_flash_chunk_samples == 0U || i2s_flash_record_error) {
		return;
	}

	bytes = i2s_flash_chunk_samples * sizeof(i2s_flash_chunk[0]);
	if (bytes < sizeof(i2s_flash_chunk)) {
		memset(&i2s_flash_chunk[i2s_flash_chunk_samples], 0,
		       sizeof(i2s_flash_chunk) - bytes);
		bytes = sizeof(i2s_flash_chunk);
	}

	key = irq_lock();
	flash_range_program(i2s_flash_record_offset,
			    (const uint8_t *)i2s_flash_chunk, bytes);
	irq_unlock(key);

	i2s_flash_record_offset += bytes;
	i2s_flash_chunk_samples = 0;
}

static void i2s_flash_record_frame(int32_t left, int32_t right)
{
	int32_t left_abs = left < 0 ? -left : left;
	int32_t right_abs = right < 0 ? -right : right;
	int16_t sample = (int16_t)(right_abs > left_abs ? right : left);

	i2s_opus_record_sample(sample);

	if (!i2s_flash_recording || i2s_flash_record_error) {
		return;
	}

	if (i2s_flash_record_samples >= i2s_flash_record_target_samples) {
		i2s_flash_record_flush();
		i2s_flash_recording = false;
		i2s_flash_record_done = true;
		return;
	}

	i2s_flash_chunk[i2s_flash_chunk_samples++] = sample;
	i2s_flash_record_samples++;
	if (i2s_flash_chunk_samples >= ARRAY_SIZE(i2s_flash_chunk)) {
		i2s_flash_record_flush();
	}
	if (i2s_flash_record_samples >= i2s_flash_record_target_samples) {
		i2s_flash_record_flush();
		i2s_flash_recording = false;
		i2s_flash_record_done = true;
	}
}

static int pio_i2s_init(uint32_t sample_rate)
{
	pio_sm_config clk_config;
	pio_sm_config rx_config;
	uint32_t bclk_pin;
	uint32_t lrclk_pin;
	uint32_t data_pin;
	uint32_t dout_pin;
	uint32_t pin_mask;
	int ret;

	if (i2s_initialized) {
		return 0;
	}

	if (!device_is_ready(i2s_pio_dev)) {
		return -ENODEV;
	}

	ret = pinctrl_apply_state(i2s_pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	i2s_pio = pio_rpi_pico_get_pio(i2s_pio_dev);
	ret = pio_rpi_pico_allocate_sm(i2s_pio_dev, &i2s_clk_sm);
	if (ret < 0) {
		return ret;
	}
	ret = pio_rpi_pico_allocate_sm(i2s_pio_dev, &i2s_rx_sm);
	if (ret < 0) {
		return ret;
	}

	bclk_pin = DT_RPI_PICO_PIO_PIN_BY_NAME(I2S_NODE, default, 0, bclk_pins, 0);
	lrclk_pin = DT_RPI_PICO_PIO_PIN_BY_NAME(I2S_NODE, default, 0, lrclk_pins, 0);
	data_pin = DT_RPI_PICO_PIO_PIN_BY_NAME(I2S_NODE, default, 0, data_pins, 0);
	dout_pin = DT_RPI_PICO_PIO_PIN_BY_NAME(I2S_NODE, default, 0, dout_pins, 0);
	i2s_bclk_pin = bclk_pin;
	i2s_lrclk_pin = lrclk_pin;
	i2s_data_pin = data_pin;
	i2s_dout_pin = dout_pin;
	pin_mask = BIT(bclk_pin) | BIT(lrclk_pin) | BIT(data_pin) | BIT(dout_pin);

	i2s_clk_offset = 0;
	i2s_rx_offset = ARRAY_SIZE(i2s_clk_instructions);
	i2s_programs_build(bclk_pin, lrclk_pin);
	if (!pio_can_add_program_at_offset(i2s_pio, &i2s_clk_program, i2s_clk_offset)) {
		return -EBUSY;
	}
	pio_add_program_at_offset(i2s_pio, &i2s_clk_program, i2s_clk_offset);
	if (!pio_can_add_program_at_offset(i2s_pio, &i2s_rx_program, i2s_rx_offset)) {
		return -EBUSY;
	}
	pio_add_program_at_offset(i2s_pio, &i2s_rx_program, i2s_rx_offset);

	pio_gpio_init(i2s_pio, bclk_pin);
	pio_gpio_init(i2s_pio, lrclk_pin);
	pio_gpio_init(i2s_pio, data_pin);
	pio_gpio_init(i2s_pio, dout_pin);
	gpio_pull_down(data_pin);
	pio_sm_set_pins_with_mask(i2s_pio, i2s_clk_sm, 0, pin_mask);
	pio_sm_set_pindirs_with_mask(i2s_pio, i2s_clk_sm,
				      BIT(bclk_pin) | BIT(lrclk_pin) |
				      (i2s_tx_enabled ? BIT(dout_pin) : 0),
				      pin_mask);

	clk_config = pio_get_default_sm_config();
	sm_config_set_sideset(&clk_config, 2, false, false);
	sm_config_set_sideset_pins(&clk_config, bclk_pin);
	sm_config_set_out_pins(&clk_config, dout_pin, 1);
	sm_config_set_out_shift(&clk_config, false, false, I2S_SLOT_BITS);
	sm_config_set_fifo_join(&clk_config, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&clk_config, i2s_clock_div(sample_rate));
	sm_config_set_wrap(&clk_config, i2s_clk_offset, i2s_clk_offset + ARRAY_SIZE(i2s_clk_instructions) - 1);

	rx_config = pio_get_default_sm_config();
	sm_config_set_in_pins(&rx_config, data_pin);
	sm_config_set_in_pin_count(&rx_config, 1);
	sm_config_set_jmp_pin(&rx_config, lrclk_pin);
	sm_config_set_in_shift(&rx_config, false, true, I2S_SLOT_BITS);
	sm_config_set_fifo_join(&rx_config, PIO_FIFO_JOIN_RX);
	sm_config_set_clkdiv(&rx_config, i2s_rx_clock_div(sample_rate));
	sm_config_set_wrap(&rx_config, i2s_rx_offset, i2s_rx_offset + ARRAY_SIZE(i2s_rx_instructions) - 1);

	pio_sm_init(i2s_pio, i2s_clk_sm, i2s_clk_offset, &clk_config);
	pio_sm_init(i2s_pio, i2s_rx_sm, i2s_rx_offset, &rx_config);

	i2s_rx_dma_chan = dma_claim_unused_channel(false);
	if (i2s_rx_dma_chan < 0) {
		return -EBUSY;
	}
	i2s_rx_dma_ctrl_chan = dma_claim_unused_channel(false);
	if (i2s_rx_dma_ctrl_chan < 0) {
		return -EBUSY;
	}
	dma_channel_set_irq0_enabled(i2s_rx_dma_chan, true);

	i2s_initialized = true;
	return 0;
}

static void pio_i2s_rx_dma_start(void)
{
	dma_channel_config_t ctrl_config = dma_channel_get_default_config(i2s_rx_dma_ctrl_chan);
	dma_channel_config_t data_config = dma_channel_get_default_config(i2s_rx_dma_chan);

	i2s_rx_dma_ctrl_blocks[0] = i2s_rx_dma_buffer[0];
	i2s_rx_dma_ctrl_blocks[1] = i2s_rx_dma_buffer[1];

	dma_channel_abort(i2s_rx_dma_chan);
	dma_channel_abort(i2s_rx_dma_ctrl_chan);
	dma_channel_acknowledge_irq0(i2s_rx_dma_chan);

	channel_config_set_read_increment(&ctrl_config, true);
	channel_config_set_write_increment(&ctrl_config, false);
	channel_config_set_ring(&ctrl_config, false, 3);
	channel_config_set_transfer_data_size(&ctrl_config, DMA_SIZE_32);
	dma_channel_configure(i2s_rx_dma_ctrl_chan, &ctrl_config,
			      &dma_hw->ch[i2s_rx_dma_chan].al2_write_addr_trig,
			      i2s_rx_dma_ctrl_blocks,
			      1,
			      false);

	channel_config_set_read_increment(&data_config, false);
	channel_config_set_write_increment(&data_config, true);
	channel_config_set_transfer_data_size(&data_config, DMA_SIZE_32);
	channel_config_set_chain_to(&data_config, i2s_rx_dma_ctrl_chan);
	channel_config_set_dreq(&data_config, pio_get_dreq(i2s_pio, i2s_rx_sm, false));

	dma_channel_configure(i2s_rx_dma_chan, &data_config,
			      NULL,
			      &i2s_pio->rxf[i2s_rx_sm],
			      dma_encode_transfer_count(I2S_DMA_WORDS),
			      false);
	dma_channel_start(i2s_rx_dma_ctrl_chan);
}

static void pio_i2s_worker(void *a, void *b, void *c)
{
	uint32_t frames = 0;
	uint32_t nonzero = 0;
	uint32_t left_sat = 0;
	uint32_t right_sat = 0;
	uint32_t left_zero = 0;
	uint32_t right_zero = 0;
	uint32_t head_count = 0;
	uint32_t rx_buffer_index = 0;
	uint32_t last_log_ms = k_uptime_get_32();
	uint32_t tx_sample = 0;
	int64_t left_sum = 0;
	int64_t right_sum = 0;
	int64_t left_abs_sum = 0;
	int64_t right_abs_sum = 0;
	int32_t left_min = INT32_MAX;
	int32_t left_max = INT32_MIN;
	int32_t right_min = INT32_MAX;
	int32_t right_max = INT32_MIN;
	uint32_t left_head[I2S_DEBUG_HEAD_WORDS] = {0};
	uint32_t right_head[I2S_DEBUG_HEAD_WORDS] = {0};
	bool have_left = false;
	int32_t pending_left = 0;
	uint32_t pending_left_raw = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (i2s_running) {
		uint32_t now_ms;
		bool rx_seen = false;

		if (i2s_tx_enabled && !pio_sm_is_tx_fifo_full(i2s_pio, i2s_clk_sm)) {
			pio_sm_put(i2s_pio, i2s_clk_sm, i2s_tone_word(tx_sample++, i2s_sample_rate));
		}

		if (!I2S_USE_RX_DMA) {
			while (!pio_sm_is_rx_fifo_empty(i2s_pio, i2s_rx_sm)) {
				uint32_t raw = pio_sm_get(i2s_pio, i2s_rx_sm);
				int32_t sample = i2s_pcm16_from_raw(raw);
				int32_t left;
				int32_t right;

				rx_seen = true;
				i2s_rx_words_seen++;
				i2s_rx_last_word = raw;

				if (!have_left) {
					pending_left = sample;
					pending_left_raw = raw;
					have_left = true;
					continue;
				}

				left = pending_left;
				right = sample;
				have_left = false;
				i2s_record_frame(left, right);
				i2s_flash_record_frame(left, right);

				if (head_count < I2S_DEBUG_HEAD_WORDS) {
					left_head[head_count] = pending_left_raw;
					right_head[head_count] = raw;
					head_count++;
				}
				if (left != 0 || right != 0) {
					nonzero++;
				}
				if (left == 0) {
					left_zero++;
				}
				if (right == 0) {
					right_zero++;
				}
				if (left == INT16_MAX || left == INT16_MIN) {
					left_sat++;
				}
				if (right == INT16_MAX || right == INT16_MIN) {
					right_sat++;
				}
				left_sum += left;
				right_sum += right;
				left_abs_sum += left < 0 ? -(int64_t)left : left;
				right_abs_sum += right < 0 ? -(int64_t)right : right;
				if (left < left_min) {
					left_min = left;
				}
				if (left > left_max) {
					left_max = left;
				}
				if (right < right_min) {
					right_min = right;
				}
				if (right > right_max) {
					right_max = right;
				}
				frames++;
			}
		} else if (dma_channel_get_irq0_status(i2s_rx_dma_chan)) {
			uint32_t *rx_buffer = i2s_rx_dma_buffer[rx_buffer_index];

			dma_channel_acknowledge_irq0(i2s_rx_dma_chan);
			rx_buffer_index ^= 1U;

			for (uint32_t i = 0; i < I2S_DMA_WORDS; i += 2U) {
				uint32_t left_raw = rx_buffer[i];
				uint32_t right_raw = rx_buffer[i + 1U];
				int32_t left = i2s_pcm16_from_raw(left_raw);
				int32_t right = i2s_pcm16_from_raw(right_raw);

				i2s_record_frame(left, right);
				i2s_flash_record_frame(left, right);
				i2s_rx_words_seen += 2U;
				i2s_rx_last_word = right_raw;
				if (head_count < I2S_DEBUG_HEAD_WORDS) {
					left_head[head_count] = left_raw;
					right_head[head_count] = right_raw;
					head_count++;
				}
				if (left != 0 || right != 0) {
					nonzero++;
				}
				if (left == 0) {
					left_zero++;
				}
				if (right == 0) {
					right_zero++;
				}
				if (left == INT16_MAX || left == INT16_MIN) {
					left_sat++;
				}
				if (right == INT16_MAX || right == INT16_MIN) {
					right_sat++;
				}
				left_sum += left;
				right_sum += right;
				left_abs_sum += left < 0 ? -(int64_t)left : left;
				right_abs_sum += right < 0 ? -(int64_t)right : right;
				if (left < left_min) {
					left_min = left;
				}
				if (left > left_max) {
					left_max = left;
				}
				if (right < right_min) {
					right_min = right;
				}
				if (right > right_max) {
					right_max = right;
				}
				frames++;
			}
		}

		now_ms = k_uptime_get_32();
		if (!i2s_dumping && !i2s_recording && !i2s_flash_recording &&
		    now_ms - last_log_ms >= 1000) {
			int32_t left_avg = frames ? (int32_t)(left_sum / frames) : 0;
			int32_t right_avg = frames ? (int32_t)(right_sum / frames) : 0;
			int32_t left_abs_avg = frames ? (int32_t)(left_abs_sum / frames) : 0;
			int32_t right_abs_avg = frames ? (int32_t)(right_abs_sum / frames) : 0;

			printk("pio_i2s mic pcm16: frames=%u nonzero=%u L=[%d,%d] avg=%d abs=%d zero=%u sat=%u R=[%d,%d] avg=%d abs=%d zero=%u sat=%u\n",
			       frames, nonzero, left_min, left_max, left_avg, left_abs_avg,
			       left_zero, left_sat, right_min, right_max, right_avg,
			       right_abs_avg, right_zero, right_sat);
			printk("pio_i2s raw: L=%08x %08x %08x %08x R=%08x %08x %08x %08x\n",
			       left_head[0], left_head[1], left_head[2], left_head[3],
			       right_head[0], right_head[1], right_head[2], right_head[3]);
			frames = 0;
			nonzero = 0;
			left_sat = 0;
			right_sat = 0;
			left_zero = 0;
			right_zero = 0;
			head_count = 0;
			left_sum = 0;
			right_sum = 0;
			left_abs_sum = 0;
			right_abs_sum = 0;
			left_min = INT32_MAX;
			left_max = INT32_MIN;
			right_min = INT32_MAX;
			right_max = INT32_MIN;
			memset(left_head, 0, sizeof(left_head));
			memset(right_head, 0, sizeof(right_head));
			last_log_ms = now_ms;
		}

		if (!rx_seen) {
			k_msleep(1);
		}
	}
}

static int pio_i2s_start(uint32_t sample_rate, bool tx_enabled)
{
	int ret;

	if (i2s_running) {
		return -EALREADY;
	}

	i2s_tx_enabled = tx_enabled;
	ret = pio_i2s_init(sample_rate);
	if (ret < 0) {
		return ret;
	}

	i2s_sample_rate = sample_rate;
	pio_sm_set_clkdiv(i2s_pio, i2s_clk_sm, i2s_clock_div(sample_rate));
	pio_sm_set_clkdiv(i2s_pio, i2s_rx_sm, i2s_rx_clock_div(sample_rate));
	pio_sm_clear_fifos(i2s_pio, i2s_clk_sm);
	pio_sm_clear_fifos(i2s_pio, i2s_rx_sm);
	if (i2s_tx_enabled) {
		pio_sm_put(i2s_pio, i2s_clk_sm, 0);
		pio_sm_put(i2s_pio, i2s_clk_sm, 0);
	}
	pio_sm_restart(i2s_pio, i2s_clk_sm);
	pio_sm_restart(i2s_pio, i2s_rx_sm);

	i2s_running = true;
	dma_channel_set_irq0_enabled(i2s_rx_dma_chan, true);
	if (I2S_USE_RX_DMA) {
		pio_i2s_rx_dma_start();
	}
	pio_enable_sm_mask_in_sync(i2s_pio, BIT(i2s_clk_sm) | BIT(i2s_rx_sm));
	i2s_thread_id = k_thread_create(&i2s_thread_data, i2s_thread_stack,
					K_THREAD_STACK_SIZEOF(i2s_thread_stack),
					pio_i2s_worker, NULL, NULL, NULL,
					K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	return 0;
}

static void pio_i2s_stop(void)
{
	if (!i2s_running) {
		return;
	}

	i2s_running = false;
	k_msleep(5);
	if (i2s_rx_dma_chan >= 0) {
		dma_channel_set_irq0_enabled(i2s_rx_dma_chan, false);
		dma_channel_abort(i2s_rx_dma_chan);
		dma_channel_acknowledge_irq0(i2s_rx_dma_chan);
	}
	if (i2s_rx_dma_ctrl_chan >= 0) {
		dma_channel_abort(i2s_rx_dma_ctrl_chan);
	}
	pio_sm_set_enabled(i2s_pio, i2s_clk_sm, false);
	pio_sm_set_enabled(i2s_pio, i2s_rx_sm, false);
	pio_sm_clear_fifos(i2s_pio, i2s_clk_sm);
	pio_sm_clear_fifos(i2s_pio, i2s_rx_sm);
	i2s_thread_id = NULL;
}

static void i2s_hex_dump_recording(const struct shell *sh)
{
	static const char hex[] = "0123456789abcdef";
	char line[I2S_RECORD_DUMP_SAMPLES_PER_LINE * sizeof(int16_t) * 2U + 1U];
	uint32_t samples = i2s_record_samples;

	i2s_dumping = true;
	k_msleep(100);
	shell_print(sh, "PCM16_HEX_BEGIN rate=%u samples=%u", i2s_record_rate, samples);
	k_msleep(20);
	for (uint32_t i = 0; i < samples; i += I2S_RECORD_DUMP_SAMPLES_PER_LINE) {
		uint32_t n = MIN(I2S_RECORD_DUMP_SAMPLES_PER_LINE, samples - i);
		uint32_t out = 0;

		for (uint32_t j = 0; j < n; j++) {
			uint16_t sample = (uint16_t)i2s_record_buffer[i + j];
			uint8_t bytes[2] = {
				(uint8_t)(sample & 0xff),
				(uint8_t)(sample >> 8),
			};

			for (uint32_t k = 0; k < ARRAY_SIZE(bytes); k++) {
				line[out++] = hex[bytes[k] >> 4];
				line[out++] = hex[bytes[k] & 0x0f];
			}
		}
		line[out] = '\0';
		shell_print(sh, "%s", line);
		k_msleep(20);
	}
	shell_print(sh, "PCM16_HEX_END");
	k_msleep(20);
	i2s_dumping = false;
}

static int i2s_opus_start_recording(uint32_t ms, int bitrate)
{
	uint32_t samples;
	size_t erase_len = I2S_OPUS_RECORD_SIZE;
	uint8_t header[I2S_OPUS_FLASH_PAGE];
	unsigned int key;
	int encoder_size;
	int ret;

	if (i2s_sample_rate != 16000U) {
		return -EINVAL;
	}

	encoder_size = opus_encoder_get_size(1);
	if (encoder_size <= 0 || encoder_size > (int)sizeof(i2s_opus_encoder_storage)) {
		i2s_opus_last_error = -ENOMEM;
		return -ENOMEM;
	}

	ret = opus_encoder_init(i2s_opus_encoder, 16000, 1, OPUS_APPLICATION_AUDIO);
	if (ret != OPUS_OK) {
		i2s_opus_last_error = ret;
		return -EIO;
	}
	opus_encoder_ctl(i2s_opus_encoder, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(i2s_opus_encoder, OPUS_SET_COMPLEXITY(1));
	opus_encoder_ctl(i2s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	samples = ((uint64_t)i2s_sample_rate * ms) / 1000U;
	if (samples == 0U) {
		return -EINVAL;
	}

	i2s_opus_recording = false;
	i2s_opus_record_done = false;
	i2s_opus_record_error = false;
	i2s_opus_last_error = 0;
	i2s_opus_bitrate = bitrate;
	i2s_opus_record_rate = i2s_sample_rate;
	i2s_opus_record_target_samples = samples;
	i2s_opus_record_samples = 0U;
	i2s_opus_record_frames = 0U;
	i2s_opus_record_offset = I2S_OPUS_RECORD_OFFSET;
	i2s_opus_record_bytes = 0U;
	i2s_opus_frame_samples = 0U;
	i2s_opus_flash_page_used = 0U;
	memset(i2s_opus_frame, 0, sizeof(i2s_opus_frame));
	memset(i2s_opus_packet, 0, sizeof(i2s_opus_packet));
	memset(i2s_opus_flash_page, 0xff, sizeof(i2s_opus_flash_page));

	key = irq_lock();
	flash_range_erase(I2S_OPUS_RECORD_OFFSET, erase_len);
	irq_unlock(key);

	memset(header, 0xff, sizeof(header));
	((uint32_t *)header)[0] = I2S_OPUS_MAGIC;
	((uint32_t *)header)[1] = 1U;
	((uint32_t *)header)[2] = i2s_sample_rate;
	((uint32_t *)header)[3] = 1U;
	((uint32_t *)header)[4] = I2S_OPUS_FRAME_SAMPLES;
	((uint32_t *)header)[5] = (uint32_t)bitrate;
	((uint32_t *)header)[6] = samples;
	((uint32_t *)header)[7] = I2S_OPUS_PACKET_MAX;
	i2s_flash_program_page(i2s_opus_record_offset, header);
	i2s_opus_record_offset += I2S_OPUS_FLASH_PAGE;
	i2s_opus_record_bytes += I2S_OPUS_FLASH_PAGE;

	i2s_opus_recording = true;
	return 0;
}

static int cmd_pio_i2s(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: pio_i2s start [sample_rate] [mic|both] | stop | status | record [ms] | dump | flash_record [ms] | opus_record [ms] [bitrate]");
		return -EINVAL;
	}

	if (strcmp(argv[1], "start") == 0) {
		uint32_t rate = I2S_DEFAULT_SAMPLE_RATE;
		bool tx_enabled = true;
		int ret;

		if (argc >= 3) {
			rate = strtoul(argv[2], NULL, 10);
		}
		if (argc >= 4) {
			tx_enabled = strcmp(argv[3], "mic") != 0;
		}

		ret = pio_i2s_start(rate, tx_enabled);
		if (ret < 0) {
			shell_error(sh, "pio_i2s start failed: %d", ret);
			return ret;
		}

		shell_print(sh, "pio_i2s started: rate=%u bclk=%u Hz mode=%s pins: din=10 bclk=11 lrclk=12 dout=13",
			    rate, rate * I2S_CHANNELS * I2S_SLOT_BITS,
			    tx_enabled ? "both" : "mic");
		return 0;
	}

	if (strcmp(argv[1], "stop") == 0) {
		pio_i2s_stop();
		shell_print(sh, "pio_i2s stopped");
		return 0;
	}

	if (strcmp(argv[1], "record") == 0) {
		uint32_t ms = 1000U;
		uint32_t samples;

		if (argc >= 3) {
			ms = strtoul(argv[2], NULL, 10);
		}
		if (!i2s_running) {
			shell_error(sh, "start I2S first: pio_i2s start 16000 mic");
			return -EAGAIN;
		}

		samples = ((uint64_t)i2s_sample_rate * ms) / 1000U;
		if (samples > ARRAY_SIZE(i2s_record_buffer)) {
			samples = ARRAY_SIZE(i2s_record_buffer);
			ms = ((uint64_t)samples * 1000U) / i2s_sample_rate;
		}

		i2s_recording = false;
		i2s_record_rate = i2s_sample_rate;
		i2s_record_target_samples = samples;
		i2s_record_samples = 0;
		memset(i2s_record_buffer, 0, sizeof(i2s_record_buffer));
		i2s_recording = true;
		shell_print(sh, "pio_i2s recording: ms=%u rate=%u target_samples=%u",
			    ms, i2s_record_rate, samples);
		return 0;
	}

	if (strcmp(argv[1], "dump") == 0) {
		if (i2s_recording) {
			shell_error(sh, "recording still active: %u/%u",
				    i2s_record_samples, i2s_record_target_samples);
			return -EBUSY;
		}
		i2s_hex_dump_recording(sh);
		return 0;
	}

	if (strcmp(argv[1], "flash_record") == 0) {
		uint32_t ms = 10000U;
		uint32_t samples;
		size_t bytes;
		size_t erase_len;
		unsigned int key;

		if (argc >= 3) {
			ms = strtoul(argv[2], NULL, 10);
		}
		if (!i2s_running) {
			shell_error(sh, "start I2S first: pio_i2s start 16000 mic");
			return -EAGAIN;
		}

		samples = ((uint64_t)i2s_sample_rate * ms) / 1000U;
		bytes = samples * sizeof(int16_t);
		if (bytes > I2S_FLASH_RECORD_SIZE) {
			bytes = I2S_FLASH_RECORD_SIZE;
			samples = bytes / sizeof(int16_t);
			ms = ((uint64_t)samples * 1000U) / i2s_sample_rate;
		}
		erase_len = ROUND_UP(bytes, 0x1000U);

		i2s_flash_recording = false;
		i2s_flash_record_done = false;
		i2s_flash_record_error = false;
		i2s_flash_record_rate = i2s_sample_rate;
		i2s_flash_record_target_samples = samples;
		i2s_flash_record_samples = 0;
		i2s_flash_record_offset = I2S_FLASH_RECORD_OFFSET;
		i2s_flash_chunk_samples = 0;
		memset(i2s_flash_chunk, 0, sizeof(i2s_flash_chunk));

		shell_print(sh, "pio_i2s flash erase: off=0x%x bytes=%u erase=%u",
			    I2S_FLASH_RECORD_OFFSET, (unsigned int)bytes,
			    (unsigned int)erase_len);
		key = irq_lock();
		flash_range_erase(I2S_FLASH_RECORD_OFFSET, erase_len);
		irq_unlock(key);

		i2s_flash_recording = true;
		shell_print(sh,
			    "pio_i2s flash recording: ms=%u rate=%u target_samples=%u xip_addr=0x%08x bytes=%u",
			    ms, i2s_flash_record_rate, samples,
			    0x10000000U + I2S_FLASH_RECORD_OFFSET,
			    (unsigned int)bytes);
		return 0;
	}

	if (strcmp(argv[1], "opus_record") == 0) {
		uint32_t ms = 10000U;
		int bitrate = I2S_OPUS_DEFAULT_BITRATE;
		int ret;

		if (argc >= 3) {
			ms = strtoul(argv[2], NULL, 10);
		}
		if (argc >= 4) {
			bitrate = strtol(argv[3], NULL, 10);
		}
		if (!i2s_running) {
			shell_error(sh, "start I2S first: pio_i2s start 16000 mic");
			return -EAGAIN;
		}
		if (bitrate < 6000 || bitrate > 64000) {
			shell_error(sh, "unsupported bitrate: %d", bitrate);
			return -EINVAL;
		}

		ret = i2s_opus_start_recording(ms, bitrate);
		if (ret < 0) {
			shell_error(sh, "opus_record start failed: %d opus_err=%d",
				    ret, i2s_opus_last_error);
			return ret;
		}

		shell_print(sh,
			    "pio_i2s opus recording: ms=%u rate=%u frame=%u bitrate=%d xip_addr=0x%08x",
			    ms, i2s_opus_record_rate, I2S_OPUS_FRAME_SAMPLES,
			    bitrate, 0x10000000U + I2S_OPUS_RECORD_OFFSET);
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		shell_print(sh, "pio_i2s %s rate=%u sm clk=%u rx=%u tx=%s",
			    i2s_running ? "running" : "stopped",
			    i2s_sample_rate,
			    (unsigned int)i2s_clk_sm,
			    (unsigned int)i2s_rx_sm,
			    i2s_tx_enabled ? "clk_fifo" : "off");
		if (i2s_initialized) {
			uint32_t clk_exec = i2s_pio->sm[i2s_clk_sm].execctrl;
			uint32_t rx_exec = i2s_pio->sm[i2s_rx_sm].execctrl;

			shell_print(sh,
				    "pio: ctrl=0x%08x clk_pc=%u rx_pc=%u clk_tx=%u rx_fifo=%u fdebug=0x%08x",
				    i2s_pio->ctrl,
				    pio_sm_get_pc(i2s_pio, i2s_clk_sm),
				    pio_sm_get_pc(i2s_pio, i2s_rx_sm),
				    pio_sm_get_tx_fifo_level(i2s_pio, i2s_clk_sm),
				    pio_sm_get_rx_fifo_level(i2s_pio, i2s_rx_sm),
				    i2s_pio->fdebug);
			shell_print(sh,
				    "clk sm: clkdiv=0x%08x exec=0x%08x wrap=%u..%u shift=0x%08x pin=0x%08x instr=0x%04x",
				    i2s_pio->sm[i2s_clk_sm].clkdiv,
				    clk_exec,
				    (clk_exec >> 7) & 0x1f,
				    (clk_exec >> 12) & 0x1f,
				    i2s_pio->sm[i2s_clk_sm].shiftctrl,
				    i2s_pio->sm[i2s_clk_sm].pinctrl,
				    i2s_pio->sm[i2s_clk_sm].instr & 0xffff);
			shell_print(sh,
				    "rx sm: clkdiv=0x%08x exec=0x%08x wrap=%u..%u shift=0x%08x pin=0x%08x instr=0x%04x words=%u last=0x%08x",
				    i2s_pio->sm[i2s_rx_sm].clkdiv,
				    rx_exec,
				    (rx_exec >> 7) & 0x1f,
				    (rx_exec >> 12) & 0x1f,
				    i2s_pio->sm[i2s_rx_sm].shiftctrl,
				    i2s_pio->sm[i2s_rx_sm].pinctrl,
				    i2s_pio->sm[i2s_rx_sm].instr & 0xffff,
				    i2s_rx_words_seen,
				    i2s_rx_last_word);
			shell_print(sh,
				    "prog: clk_off=%u rx_off=%u clk=[%04x %04x %04x %04x %04x %04x]",
				    i2s_clk_offset, i2s_rx_offset,
				    i2s_clk_instructions[0], i2s_clk_instructions[1],
				    i2s_clk_instructions[2], i2s_clk_instructions[3],
				    i2s_clk_instructions[4], i2s_clk_instructions[5]);
			shell_print(sh,
				    "prog rx=[%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x]",
				    i2s_rx_instructions[0], i2s_rx_instructions[1],
				    i2s_rx_instructions[2], i2s_rx_instructions[3],
				    i2s_rx_instructions[4], i2s_rx_instructions[5],
				    i2s_rx_instructions[6], i2s_rx_instructions[7],
				    i2s_rx_instructions[8], i2s_rx_instructions[9],
				    i2s_rx_instructions[10], i2s_rx_instructions[11],
				    i2s_rx_instructions[12], i2s_rx_instructions[13],
				    i2s_rx_instructions[14], i2s_rx_instructions[15],
				    i2s_rx_instructions[16], i2s_rx_instructions[17],
				    i2s_rx_instructions[18], i2s_rx_instructions[19],
				    i2s_rx_instructions[20], i2s_rx_instructions[21],
				    i2s_rx_instructions[22], i2s_rx_instructions[23]);
			shell_print(sh,
				    "dma: ch=%d ctrl=%d busy=%u tcr=%u irq=%u ctrl_busy=%u ctrl_tcr=%u",
				    i2s_rx_dma_chan, i2s_rx_dma_ctrl_chan,
				    !!(dma_hw->ch[i2s_rx_dma_chan].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS),
				    dma_hw->ch[i2s_rx_dma_chan].transfer_count,
				    dma_channel_get_irq0_status(i2s_rx_dma_chan),
				    !!(dma_hw->ch[i2s_rx_dma_ctrl_chan].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS),
				    dma_hw->ch[i2s_rx_dma_ctrl_chan].transfer_count);
			shell_print(sh, "gpio: din%u=%d bclk%u=%d lrclk%u=%d dout%u=%d",
				    i2s_data_pin, gpio_get(i2s_data_pin),
				    i2s_bclk_pin, gpio_get(i2s_bclk_pin),
				    i2s_lrclk_pin, gpio_get(i2s_lrclk_pin),
				    i2s_dout_pin, gpio_get(i2s_dout_pin));
			shell_print(sh,
				    "flash_rec: active=%u done=%u err=%u samples=%u/%u off=0x%x xip=0x%08x",
				    i2s_flash_recording, i2s_flash_record_done,
				    i2s_flash_record_error,
				    i2s_flash_record_samples,
				    i2s_flash_record_target_samples,
				    i2s_flash_record_offset,
				    0x10000000U + I2S_FLASH_RECORD_OFFSET);
			shell_print(sh,
				    "opus_rec: active=%u done=%u err=%u opus_err=%d samples=%u/%u frames=%u bytes=%u off=0x%x xip=0x%08x bitrate=%d",
				    i2s_opus_recording, i2s_opus_record_done,
				    i2s_opus_record_error, i2s_opus_last_error,
				    i2s_opus_record_samples,
				    i2s_opus_record_target_samples,
				    i2s_opus_record_frames,
				    i2s_opus_record_bytes,
				    i2s_opus_record_offset,
				    0x10000000U + I2S_OPUS_RECORD_OFFSET,
				    i2s_opus_bitrate);
		}
		return 0;
	}

	shell_error(sh, "unknown command: %s", argv[1]);
	return -EINVAL;
}

SHELL_CMD_REGISTER(pio_i2s, NULL, "PIO I2S MIC/SPK prototype", cmd_pio_i2s);
