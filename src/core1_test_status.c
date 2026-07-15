// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <hardware/address_mapped.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/structs/iobank0.h>
#include <hardware/structs/padsbank0.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/timer.h>

#include <opus.h>

#include "audio_pio.pio.h"
#include "pico_clip_core1_test_shared.h"

#define CPU1_SRAM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CPU1_SRAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram_cpu1_partition))

#define CORE1_FIFO_TIMEOUT_LOOPS 100000U
#define CORE1_I2C_DELAY_LOOPS 80U

#define ES8311_ADDR 0x18U
#define ES8311_RESET_REG00 0x00U
#define ES8311_CLK_MANAGER_REG01 0x01U
#define ES8311_CLK_MANAGER_REG02 0x02U
#define ES8311_CLK_MANAGER_REG03 0x03U
#define ES8311_CLK_MANAGER_REG04 0x04U
#define ES8311_CLK_MANAGER_REG05 0x05U
#define ES8311_CLK_MANAGER_REG06 0x06U
#define ES8311_CLK_MANAGER_REG07 0x07U
#define ES8311_CLK_MANAGER_REG08 0x08U
#define ES8311_SDPIN_REG09 0x09U
#define ES8311_SDPOUT_REG0A 0x0aU
#define ES8311_SYSTEM_REG0D 0x0dU
#define ES8311_SYSTEM_REG0E 0x0eU
#define ES8311_SYSTEM_REG12 0x12U
#define ES8311_SYSTEM_REG13 0x13U
#define ES8311_SYSTEM_REG14 0x14U
#define ES8311_ADC_REG16 0x16U
#define ES8311_ADC_REG17 0x17U
#define ES8311_ADC_REG1C 0x1cU
#define ES8311_DAC_REG31 0x31U
#define ES8311_DAC_REG32 0x32U
#define ES8311_DAC_REG37 0x37U

#define ES8311_SAMPLE_RATE 24000U
#define ES8311_MCLK_HZ (ES8311_SAMPLE_RATE * 256U)
#define ES8311_PA_CTRL_GPIO 0U
#define ES8311_DOUT_GPIO 1U
#define ES8311_DIN_GPIO 2U
#define ES8311_SDA_GPIO 6U
#define ES8311_SCL_GPIO 7U
#define ES8311_MCLK_GPIO 3U
#define ES8311_BCLK_GPIO 4U
#define ES8311_LRCLK_GPIO 5U
#define ES8311_BAT_EN_GPIO 28U
#define ES8311_BIRTHDAY_SAMPLES 124800U
#define ES8311_LOOPBACK_SAMPLES 1024U
#define ES8311_BIRTHDAY_DAC_VOLUME 0x98U
#define ES8311_BIRTHDAY_ADC_GAIN 0x03U
#define ES8311_LOOPBACK_DAC_VOLUME ES8311_BIRTHDAY_DAC_VOLUME
#define ES8311_LOOPBACK_ADC_GAIN ES8311_BIRTHDAY_ADC_GAIN
/* Measured sizes for this mono fixed-point Opus build are 15228 and 18340
 * bytes.  Keep aligned headroom without unnecessarily consuming the core1
 * stack, which shares this 64 KiB SRAM partition and grows down from its end.
 */
#define CORE1_OPUS_ENCODER_BYTES 15616U
#define CORE1_OPUS_DECODER_BYTES 18752U
#define CORE1_WORK_BASE (CPU1_SRAM_ADDR + 0x100U)
#define CORE1_OPUS_ENCODER_ADDR CORE1_WORK_BASE
#define CORE1_OPUS_DECODER_ADDR (CORE1_OPUS_ENCODER_ADDR + CORE1_OPUS_ENCODER_BYTES)
#define CORE1_OPUS_PCM_ADDR (CORE1_OPUS_DECODER_ADDR + CORE1_OPUS_DECODER_BYTES)
#define CORE1_OPUS_PCM_ALT_ADDR \
	(CORE1_OPUS_PCM_ADDR + PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES * sizeof(int16_t))
#define CORE1_OPUS_SPK_PCM_ADDR \
	(CORE1_OPUS_PCM_ALT_ADDR + PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES * sizeof(int16_t))
#define CORE1_LOOPBACK_BUFFER_ADDR \
	(CORE1_OPUS_SPK_PCM_ADDR + PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES * sizeof(int16_t))
#define CORE1_OPUS_PACKET_ADDR (CORE1_LOOPBACK_BUFFER_ADDR + ES8311_LOOPBACK_SAMPLES * sizeof(int16_t))
#define CORE1_WORK_END (CORE1_OPUS_PACKET_ADDR + PICO_CLIP_CORE1_OPUS_MAX_PACKET)

#define CORE1_AUDIO_PROGRESS_GPIO 1U
#define CORE1_AUDIO_PROGRESS_I2C_PINS 2U
#define CORE1_AUDIO_PROGRESS_CODEC_INIT 3U
#define CORE1_AUDIO_PROGRESS_MCLK_PIO 4U
#define CORE1_AUDIO_PROGRESS_I2S_PIO 5U
#define CORE1_AUDIO_PROGRESS_DMA_CLAIM 6U
#define CORE1_AUDIO_PROGRESS_READY 7U
#define CORE1_AUDIO_PROGRESS_DMA_PLAY 8U
#define CORE1_AUDIO_PROGRESS_DONE 9U
#define CORE1_AUDIO_PROGRESS_DIN_PIO 10U
#define CORE1_AUDIO_PROGRESS_LOOPBACK 11U
#define CORE1_AUDIO_PROGRESS_OPUS 12U

extern const int16_t Happy_birsday[];

static bool core1_audio_ready;
static bool core1_audio_pio_ready;
static bool core1_audio_din_ready;
static bool core1_opus_ready;
static uint core1_audio_dma_chan = UINT_MAX;
static uint core1_audio_rx_dma_chan = UINT_MAX;

static inline bool core1_fifo_write_ready(void)
{
	return (sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS) != 0U;
}

static inline bool core1_fifo_read_valid(void)
{
	return (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) != 0U;
}

static void core1_fifo_drain(void)
{
	while (core1_fifo_read_valid()) {
		(void)sio_hw->fifo_rd;
	}
}

static void core1_fifo_clear_irq(void)
{
	sio_hw->fifo_st = 0xff;
}

static int core1_fifo_put(uint32_t value)
{
	for (uint32_t i = 0; i < CORE1_FIFO_TIMEOUT_LOOPS; i++) {
		if (core1_fifo_write_ready()) {
			sio_hw->fifo_wr = value;
			__SEV();
			return 0;
		}

		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int core1_fifo_pop(uint32_t *value)
{
	for (uint32_t i = 0; i < CORE1_FIFO_TIMEOUT_LOOPS; i++) {
		if (core1_fifo_read_valid()) {
			*value = sio_hw->fifo_rd;
			return 0;
		}

		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int core1_reset_with_timeout(const struct shell *sh)
{
	uint32_t val;

	core1_fifo_drain();
	core1_fifo_clear_irq();

	if (sh != NULL) {
		shell_print(sh, "core1 start: reset enter fifo_st=0x%08x frce_off=0x%08x",
			    sio_hw->fifo_st, psm_hw->frce_off);
	}

	if (sh != NULL) {
		shell_print(sh, "core1 start: force PROC1 off");
	}
	hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
	for (uint32_t i = 0; i < CORE1_FIFO_TIMEOUT_LOOPS; i++) {
		if ((psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS) != 0U) {
			if (sh != NULL) {
				shell_print(sh,
					    "core1 start: PROC1 off ack after %u polls frce_off=0x%08x",
					    i, psm_hw->frce_off);
			}
			break;
		}

		if (i == CORE1_FIFO_TIMEOUT_LOOPS - 1U) {
			if (sh != NULL) {
				shell_error(sh, "core1 start: timeout forcing PROC1 off");
			}
			return -ETIMEDOUT;
		}
	}

	if (sh != NULL) {
		shell_print(sh, "core1 start: release PROC1");
	}
	hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
	if (sh != NULL) {
		shell_print(sh, "core1 start: wait reset token fifo_st=0x%08x frce_off=0x%08x",
			    sio_hw->fifo_st, psm_hw->frce_off);
	}
	if (core1_fifo_pop(&val) != 0) {
		if (sh != NULL) {
			shell_error(sh, "core1 start: timeout waiting reset token fifo_st=0x%08x",
				    sio_hw->fifo_st);
		}
		return -ETIMEDOUT;
	}

	if (sh != NULL) {
		shell_print(sh, "core1 start: reset token=0x%08x", val);
	}
	if (val != 0U) {
		if (sh != NULL) {
			shell_error(sh, "core1 start: reset token was 0x%08x", val);
		}
		return -EIO;
	}

	return 0;
}

static int core1_boot_no_final_wait(const struct shell *sh, uint32_t vector_table,
				    uint32_t sp, uint32_t pc)
{
	const uint32_t cmds[] = {0, 0, 1, vector_table, sp, pc};

	for (uint32_t seq = 0; seq < ARRAY_SIZE(cmds); seq++) {
		uint32_t cmd = cmds[seq];
		uint32_t rsp;
		int ret;

		if (cmd == 0U) {
			core1_fifo_drain();
			core1_fifo_clear_irq();
			__SEV();
		}

		if (sh != NULL) {
			shell_print(sh, "core1 nowait: send seq=%u cmd=0x%08x", seq, cmd);
			k_sleep(K_MSEC(20));
		}

		ret = core1_fifo_put(cmd);
		if (ret != 0) {
			if (sh != NULL) {
				shell_error(sh, "core1 nowait: timeout sending seq=%u", seq);
			}
			return ret;
		}

		if (seq == ARRAY_SIZE(cmds) - 1U) {
			if (sh != NULL) {
				shell_print(sh, "core1 nowait: final PC sent, not waiting for echo");
				k_sleep(K_MSEC(100));
				shell_print(sh, "core1 nowait: CPU0 still running after final PC");
			}
			return 0;
		}

		ret = core1_fifo_pop(&rsp);
		if (ret != 0) {
			if (sh != NULL) {
				shell_error(sh, "core1 nowait: timeout receiving seq=%u", seq);
			}
			return ret;
		}

		if (rsp != cmd) {
			if (sh != NULL) {
				shell_error(sh,
					    "core1 nowait: bad rsp seq=%u cmd=0x%08x rsp=0x%08x",
					    seq, cmd, rsp);
			}
			return -EIO;
		}
	}

	return 0;
}

static void core1_delay_loops(uint32_t loops)
{
	for (volatile uint32_t i = 0; i < loops; i++) {
	}
}

static void core1_i2c_delay(void)
{
	core1_delay_loops(CORE1_I2C_DELAY_LOOPS);
}

static void core1_gpio_set_func_sio(uint gpio)
{
	iobank0_hw->io[gpio].ctrl = GPIO_FUNC_SIO;
}

static void core1_gpio_set_func_pio(uint gpio, PIO pio)
{
	iobank0_hw->io[gpio].ctrl = PIO_FUNCSEL_NUM(pio, gpio);
}

static void core1_gpio_pad_init(uint gpio, bool pull_up)
{
	uint32_t pad = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_SCHMITT_BITS;

	if (pull_up) {
		pad |= PADS_BANK0_GPIO0_PUE_BITS;
	}
	padsbank0_hw->io[gpio] = pad;
}

static void core1_gpio_put(uint gpio, bool value)
{
	if (value) {
		sio_hw->gpio_set = BIT(gpio);
	} else {
		sio_hw->gpio_clr = BIT(gpio);
	}
}

static void core1_gpio_set_dir(uint gpio, bool out)
{
	if (out) {
		sio_hw->gpio_oe_set = BIT(gpio);
	} else {
		sio_hw->gpio_oe_clr = BIT(gpio);
	}
}

static bool core1_gpio_get(uint gpio)
{
	return (sio_hw->gpio_in & BIT(gpio)) != 0U;
}

static void core1_i2c_sda_high(void)
{
	core1_gpio_set_dir(ES8311_SDA_GPIO, false);
	core1_i2c_delay();
}

static void core1_i2c_sda_low(void)
{
	core1_gpio_put(ES8311_SDA_GPIO, false);
	core1_gpio_set_dir(ES8311_SDA_GPIO, true);
	core1_i2c_delay();
}

static void core1_i2c_scl_high(void)
{
	core1_gpio_set_dir(ES8311_SCL_GPIO, false);
	for (uint32_t i = 0; i < 1000U && !core1_gpio_get(ES8311_SCL_GPIO); i++) {
		core1_i2c_delay();
	}
	core1_i2c_delay();
}

static void core1_i2c_scl_low(void)
{
	core1_gpio_put(ES8311_SCL_GPIO, false);
	core1_gpio_set_dir(ES8311_SCL_GPIO, true);
	core1_i2c_delay();
}

static void core1_i2c_init_pins(void)
{
	core1_gpio_set_func_sio(ES8311_SDA_GPIO);
	core1_gpio_set_func_sio(ES8311_SCL_GPIO);
	core1_gpio_pad_init(ES8311_SDA_GPIO, true);
	core1_gpio_pad_init(ES8311_SCL_GPIO, true);
	core1_gpio_put(ES8311_SDA_GPIO, false);
	core1_gpio_put(ES8311_SCL_GPIO, false);
	core1_gpio_set_dir(ES8311_SDA_GPIO, false);
	core1_gpio_set_dir(ES8311_SCL_GPIO, false);
	core1_i2c_delay();
}

static void core1_i2c_start(void)
{
	core1_i2c_sda_high();
	core1_i2c_scl_high();
	core1_i2c_sda_low();
	core1_i2c_scl_low();
}

static void core1_i2c_stop(void)
{
	core1_i2c_sda_low();
	core1_i2c_scl_high();
	core1_i2c_sda_high();
}

static bool core1_i2c_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		if ((value & BIT(bit)) != 0U) {
			core1_i2c_sda_high();
		} else {
			core1_i2c_sda_low();
		}
		core1_i2c_scl_high();
		core1_i2c_scl_low();
	}

	core1_i2c_sda_high();
	core1_i2c_scl_high();
	bool ack = !core1_gpio_get(ES8311_SDA_GPIO);
	core1_i2c_scl_low();
	return ack;
}

static uint8_t core1_i2c_read_byte(bool ack)
{
	uint8_t value = 0;

	core1_i2c_sda_high();
	for (int bit = 7; bit >= 0; bit--) {
		core1_i2c_scl_high();
		if (core1_gpio_get(ES8311_SDA_GPIO)) {
			value |= BIT(bit);
		}
		core1_i2c_scl_low();
	}

	if (ack) {
		core1_i2c_sda_low();
	} else {
		core1_i2c_sda_high();
	}
	core1_i2c_scl_high();
	core1_i2c_scl_low();
	core1_i2c_sda_high();
	return value;
}

static int core1_es8311_write(uint8_t reg, uint8_t value)
{
	core1_i2c_start();
	if (!core1_i2c_write_byte((ES8311_ADDR << 1) | 0U) ||
	    !core1_i2c_write_byte(reg) ||
	    !core1_i2c_write_byte(value)) {
		core1_i2c_stop();
		return -EIO;
	}
	core1_i2c_stop();
	core1_delay_loops(20000U);
	return 0;
}

static int core1_es8311_read(uint8_t reg, uint8_t *value)
{
	core1_i2c_start();
	if (!core1_i2c_write_byte((ES8311_ADDR << 1) | 0U) ||
	    !core1_i2c_write_byte(reg)) {
		core1_i2c_stop();
		return -EIO;
	}

	core1_i2c_start();
	if (!core1_i2c_write_byte((ES8311_ADDR << 1) | 1U)) {
		core1_i2c_stop();
		return -EIO;
	}
	*value = core1_i2c_read_byte(false);
	core1_i2c_stop();
	core1_delay_loops(20000U);
	return 0;
}

static int core1_es8311_update(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t old_value;
	int ret = core1_es8311_read(reg, &old_value);

	if (ret != 0) {
		return ret;
	}
	return core1_es8311_write(reg, (old_value & ~mask) | (value & mask));
}

static int core1_es8311_codec_init(void)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();
	uint8_t reg00;
	uint8_t reg02;
	uint8_t reg06;
	int ret;

	shm->audio_progress = CORE1_AUDIO_PROGRESS_CODEC_INIT;
	ret = core1_es8311_write(ES8311_RESET_REG00, 0x1f);
	core1_delay_loops(800000U);
	ret |= core1_es8311_write(ES8311_RESET_REG00, 0x00);
	ret |= core1_es8311_write(ES8311_RESET_REG00, 0x80);

	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG01, 0x3f);
	ret |= core1_es8311_read(ES8311_CLK_MANAGER_REG06, &reg06);
	reg06 &= (uint8_t)~BIT(5);
	reg06 |= 0x03;
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG06, reg06);

	ret |= core1_es8311_read(ES8311_CLK_MANAGER_REG02, &reg02);
	reg02 &= 0x07;
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG02, reg02);
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG03, 0x10);
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG04, 0x10);
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG05, 0x00);
	ret |= core1_es8311_read(ES8311_CLK_MANAGER_REG06, &reg06);
	reg06 &= 0xe0;
	reg06 |= 0x07;
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG06, reg06);
	ret |= core1_es8311_update(ES8311_CLK_MANAGER_REG07, 0x3f, 0x00);
	ret |= core1_es8311_write(ES8311_CLK_MANAGER_REG08, 0xff);

	ret |= core1_es8311_read(ES8311_RESET_REG00, &reg00);
	reg00 |= 0x40;
	ret |= core1_es8311_write(ES8311_RESET_REG00, reg00);
	ret |= core1_es8311_write(ES8311_SDPIN_REG09, 0x0c);
	ret |= core1_es8311_write(ES8311_SDPOUT_REG0A, 0x0c);

	ret |= core1_es8311_write(ES8311_SYSTEM_REG0D, 0x01);
	ret |= core1_es8311_write(ES8311_SYSTEM_REG0E, 0x02);
	ret |= core1_es8311_write(ES8311_SYSTEM_REG12, 0x00);
	ret |= core1_es8311_write(ES8311_SYSTEM_REG13, 0x10);
	ret |= core1_es8311_write(ES8311_ADC_REG1C, 0x6a);
	ret |= core1_es8311_write(ES8311_DAC_REG37, 0x08);
	ret |= core1_es8311_write(ES8311_ADC_REG17, 0xff);
	ret |= core1_es8311_write(ES8311_SYSTEM_REG14, 0x1a);
	ret |= core1_es8311_write(ES8311_ADC_REG16, ES8311_BIRTHDAY_ADC_GAIN);
	ret |= core1_es8311_write(ES8311_DAC_REG32, ES8311_BIRTHDAY_DAC_VOLUME);
	ret |= core1_es8311_update(ES8311_DAC_REG31, 0x60, 0x00);

	return ret < 0 ? ret : 0;
}

static float core1_mclk_clock_div(void)
{
	return (float)clock_get_hz(clk_sys) / (float)(ES8311_MCLK_HZ * mclk_pio_program.length);
}

static int core1_es8311_hw_init(void)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	if (core1_audio_ready) {
		return 0;
	}

	shm->audio_progress = CORE1_AUDIO_PROGRESS_GPIO;
	core1_gpio_set_func_sio(ES8311_PA_CTRL_GPIO);
	core1_gpio_pad_init(ES8311_PA_CTRL_GPIO, false);
	core1_gpio_put(ES8311_PA_CTRL_GPIO, true);
	core1_gpio_set_dir(ES8311_PA_CTRL_GPIO, true);

	core1_gpio_set_func_sio(ES8311_BAT_EN_GPIO);
	core1_gpio_pad_init(ES8311_BAT_EN_GPIO, false);
	core1_gpio_put(ES8311_BAT_EN_GPIO, true);
	core1_gpio_set_dir(ES8311_BAT_EN_GPIO, true);

	shm->audio_progress = CORE1_AUDIO_PROGRESS_I2C_PINS;
	core1_i2c_init_pins();

	int ret = core1_es8311_codec_init();

	if (ret != 0) {
		return ret;
	}

	if (!core1_audio_pio_ready) {
		PIO mclk_pio = pio1;
		PIO i2s_pio = pio2;
		uint mclk_sm = 2;
		uint i2s_sm = 0;
		uint offset;

		shm->audio_progress = CORE1_AUDIO_PROGRESS_MCLK_PIO;
		core1_gpio_set_func_pio(ES8311_MCLK_GPIO, mclk_pio);
		core1_gpio_pad_init(ES8311_MCLK_GPIO, false);
		pio_sm_claim(mclk_pio, mclk_sm);
		offset = pio_add_program(mclk_pio, &mclk_pio_program);
		mclk_pio_program_init(mclk_pio, mclk_sm, offset, ES8311_MCLK_GPIO);
		pio_sm_set_clkdiv(mclk_pio, mclk_sm, core1_mclk_clock_div());
		pio_sm_clear_fifos(mclk_pio, mclk_sm);
		pio_sm_restart(mclk_pio, mclk_sm);
		pio_sm_set_enabled(mclk_pio, mclk_sm, true);

		shm->audio_progress = CORE1_AUDIO_PROGRESS_I2S_PIO;
		core1_gpio_set_func_pio(ES8311_DOUT_GPIO, i2s_pio);
		core1_gpio_set_func_pio(ES8311_BCLK_GPIO, i2s_pio);
		core1_gpio_set_func_pio(ES8311_LRCLK_GPIO, i2s_pio);
		core1_gpio_pad_init(ES8311_DOUT_GPIO, false);
		core1_gpio_pad_init(ES8311_BCLK_GPIO, false);
		core1_gpio_pad_init(ES8311_LRCLK_GPIO, false);
		pio_sm_claim(i2s_pio, i2s_sm);
		offset = pio_add_program(i2s_pio, &audio_pio_program);
		audio_pio_program_init(i2s_pio, i2s_sm, offset,
				       ES8311_DOUT_GPIO, ES8311_LRCLK_GPIO);
		pio_sm_set_clkdiv(i2s_pio, i2s_sm, 1.0f);
		pio_sm_clear_fifos(i2s_pio, i2s_sm);
		pio_sm_restart(i2s_pio, i2s_sm);
		pio_sm_set_enabled(i2s_pio, i2s_sm, true);

		shm->audio_progress = CORE1_AUDIO_PROGRESS_DMA_CLAIM;
		int dma_chan = dma_claim_unused_channel(false);

		if (dma_chan < 0) {
			return -EBUSY;
		}
		core1_audio_dma_chan = (uint)dma_chan;
		core1_audio_pio_ready = true;
	}

	shm->audio_progress = CORE1_AUDIO_PROGRESS_READY;
	core1_audio_ready = true;
	return 0;
}

static int core1_es8311_din_init(volatile struct pico_clip_core1_test_shared *shm)
{
	PIO din_pio = pio1;
	uint din_sm = 1;
	uint offset;

	if (core1_audio_din_ready) {
		return 0;
	}

	shm->audio_progress = CORE1_AUDIO_PROGRESS_DIN_PIO;
	core1_gpio_set_func_pio(ES8311_DIN_GPIO, din_pio);
	core1_gpio_set_func_pio(ES8311_BCLK_GPIO, din_pio);
	core1_gpio_set_func_pio(ES8311_LRCLK_GPIO, din_pio);
	core1_gpio_pad_init(ES8311_DIN_GPIO, false);
	core1_gpio_pad_init(ES8311_BCLK_GPIO, false);
	core1_gpio_pad_init(ES8311_LRCLK_GPIO, false);
	pio_sm_claim(din_pio, din_sm);
	offset = pio_add_program(din_pio, &read_pio_program);
	read_pio_program_init(din_pio, din_sm, offset, ES8311_DIN_GPIO, ES8311_LRCLK_GPIO);
	pio_sm_set_clkdiv(din_pio, din_sm, 1.0f);
	pio_sm_clear_fifos(din_pio, din_sm);
	pio_sm_restart(din_pio, din_sm);
	pio_sm_set_enabled(din_pio, din_sm, true);
	core1_audio_din_ready = true;
	return 0;
}

static void core1_service_test_command(volatile struct pico_clip_core1_test_shared *shm)
{
	uint32_t cmd_seq = shm->cpu0_cmd_seq;

	if (cmd_seq != shm->cpu1_ack_seq) {
		shm->cpu1_ack_seq = cmd_seq;
		shm->cpu1_response_value = shm->cpu0_cmd_value + 1U;
	}
}

static bool core1_audio_stop_requested(volatile struct pico_clip_core1_test_shared *shm)
{
	if (shm->cpu0_audio_cmd_seq != shm->cpu1_audio_ack_seq &&
	    shm->cpu0_audio_cmd == PICO_CLIP_CORE1_AUDIO_CMD_STOP) {
		shm->cpu1_audio_ack_seq = shm->cpu0_audio_cmd_seq;
		return true;
	}

	return false;
}

static uint32_t core1_checksum32(const uint8_t *data, uint32_t len)
{
	uint32_t checksum = 0x811c9dc5u;

	for (uint32_t i = 0; i < len; i++) {
		checksum ^= data[i];
		checksum *= 16777619u;
	}

	return checksum;
}

static OpusEncoder *core1_opus_encoder(void)
{
	return (OpusEncoder *)CORE1_OPUS_ENCODER_ADDR;
}

static OpusDecoder *core1_opus_decoder(void)
{
	return (OpusDecoder *)CORE1_OPUS_DECODER_ADDR;
}

static int16_t *core1_opus_pcm_frame(void)
{
	return (int16_t *)CORE1_OPUS_PCM_ADDR;
}

static int16_t *core1_opus_pcm_alt_frame(void)
{
	return (int16_t *)CORE1_OPUS_PCM_ALT_ADDR;
}

static int16_t *core1_opus_spk_frame(void)
{
	return (int16_t *)CORE1_OPUS_SPK_PCM_ADDR;
}

static uint8_t *core1_opus_packet_tmp(void)
{
	return (uint8_t *)CORE1_OPUS_PACKET_ADDR;
}

static int core1_opus_init(volatile struct pico_clip_core1_test_shared *shm)
{
	int ret;

	if (CORE1_WORK_END > CPU1_SRAM_ADDR + CPU1_SRAM_SIZE - 8192U) {
		return -ENOMEM;
	}

	if (core1_opus_ready) {
		return 0;
	}

	ret = opus_encoder_get_size(PICO_CLIP_CORE1_OPUS_CHANNELS);
	shm->opus_encoder_size = ret > 0 ? (uint32_t)ret : 0U;
	if (ret <= 0 || ret > (int)CORE1_OPUS_ENCODER_BYTES) {
		return -ENOMEM;
	}

	ret = opus_encoder_init(core1_opus_encoder(), PICO_CLIP_CORE1_OPUS_SAMPLE_RATE,
				PICO_CLIP_CORE1_OPUS_CHANNELS, OPUS_APPLICATION_AUDIO);
	if (ret != OPUS_OK) {
		return ret;
	}
	(void)opus_encoder_ctl(core1_opus_encoder(),
			       OPUS_SET_BITRATE(PICO_CLIP_CORE1_OPUS_BITRATE));
	(void)opus_encoder_ctl(core1_opus_encoder(), OPUS_SET_COMPLEXITY(0));
	(void)opus_encoder_ctl(core1_opus_encoder(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	ret = opus_decoder_get_size(PICO_CLIP_CORE1_OPUS_CHANNELS);
	shm->opus_decoder_size = ret > 0 ? (uint32_t)ret : 0U;
	if (ret <= 0 || ret > (int)CORE1_OPUS_DECODER_BYTES) {
		return -ENOMEM;
	}

	ret = opus_decoder_init(core1_opus_decoder(), PICO_CLIP_CORE1_OPUS_SAMPLE_RATE,
				PICO_CLIP_CORE1_OPUS_CHANNELS);
	if (ret != OPUS_OK) {
		return ret;
	}

	shm->opus_bitrate = PICO_CLIP_CORE1_OPUS_BITRATE;
	shm->audio_flags |= PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY |
			    PICO_CLIP_CORE1_AUDIO_FLAG_SPK_READY;
	core1_opus_ready = true;
	return 0;
}

static int core1_capture_opus_frame(volatile struct pico_clip_core1_test_shared *shm,
				    const dma_channel_config *rx_config,
				    int16_t *pcm)
{
	PIO rx_pio = pio1;
	uint rx_sm = 1;
	int16_t min_sample = INT16_MAX;
	int16_t max_sample = INT16_MIN;
	uint32_t nonzero = 0;

	pio_sm_clear_fifos(rx_pio, rx_sm);
	dma_channel_configure(core1_audio_rx_dma_chan,
			      rx_config,
			      pcm,
			      &rx_pio->rxf[rx_sm],
			      PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES,
			      true);

	while (dma_channel_is_busy(core1_audio_rx_dma_chan)) {
		shm->cpu1_heartbeat++;
		shm->audio_heartbeat++;
		core1_service_test_command(shm);
		if (core1_audio_stop_requested(shm)) {
			dma_channel_abort(core1_audio_rx_dma_chan);
			pio_sm_clear_fifos(rx_pio, rx_sm);
			return -ECANCELED;
		}
	}

	for (uint32_t i = 0; i < PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES; i++) {
		int16_t sample = pcm[i];

		if (sample != 0) {
			nonzero++;
		}
		if (sample < min_sample) {
			min_sample = sample;
		}
		if (sample > max_sample) {
			max_sample = sample;
		}
	}

	shm->audio_sample_min = min_sample;
	shm->audio_sample_max = max_sample;
	shm->audio_sample_nonzero = nonzero;
	return 0;
}

static int core1_play_pcm_frame(volatile struct pico_clip_core1_test_shared *shm,
				const dma_channel_config *tx_config,
				const int16_t *pcm, uint32_t samples)
{
	PIO tx_pio = pio2;
	uint tx_sm = 0;

	dma_channel_configure(core1_audio_dma_chan,
			      tx_config,
			      &tx_pio->txf[tx_sm],
			      pcm,
			      samples,
			      true);

	while (dma_channel_is_busy(core1_audio_dma_chan)) {
		shm->cpu1_heartbeat++;
		shm->audio_heartbeat++;
		core1_service_test_command(shm);
		if (core1_audio_stop_requested(shm)) {
			dma_channel_abort(core1_audio_dma_chan);
			pio_sm_clear_fifos(tx_pio, tx_sm);
			return -ECANCELED;
		}
	}

	return 0;
}

static int core1_decode_speaker_packet(volatile struct pico_clip_core1_test_shared *shm,
				       const dma_channel_config *tx_config)
{
	uint32_t read_seq = shm->spk_opus_read_seq;
	uint32_t write_seq = shm->spk_opus_write_seq;
	uint32_t slot;
	uint32_t slot_seq;
	uint32_t len;
	int ret;

	if (read_seq == write_seq) {
		return 0;
	}

	slot = read_seq % PICO_CLIP_CORE1_SPK_OPUS_QUEUE;
	slot_seq = shm->spk_opus_slot_seq[slot];
	if (slot_seq != read_seq + 1U) {
		return 0;
	}

	len = shm->spk_opus_len[slot];
	if (len == 0U || len > PICO_CLIP_CORE1_OPUS_MAX_PACKET) {
		shm->spk_opus_read_seq = read_seq + 1U;
		shm->spk_opus_dropped++;
		return -EINVAL;
	}

	memcpy(core1_opus_packet_tmp(), (const void *)shm->spk_opus_packet[slot], len);
	if (shm->spk_opus_slot_seq[slot] != slot_seq) {
		return 0;
	}
	shm->spk_opus_read_seq = read_seq + 1U;

	ret = opus_decode(core1_opus_decoder(), core1_opus_packet_tmp(), (opus_int32)len,
			  core1_opus_spk_frame(), PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, 0);
	if (ret < 0) {
		shm->audio_error = ret;
		shm->spk_opus_dropped++;
		return ret;
	}

	ret = core1_play_pcm_frame(shm, tx_config, core1_opus_spk_frame(), (uint32_t)ret);
	if (ret == 0) {
		shm->opus_decode_count++;
		shm->audio_play_count++;
	}
	return ret;
}

static int core1_publish_mic_opus(volatile struct pico_clip_core1_test_shared *shm,
				  int16_t *pcm, uint32_t *opus_seq)
{
	uint8_t *packet = core1_opus_packet_tmp();
	int ret;

	ret = opus_encode(core1_opus_encoder(), pcm, PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES,
			  packet, PICO_CLIP_CORE1_OPUS_MAX_PACKET);
	if (ret < 0) {
		shm->audio_error = ret;
		shm->opus_dropped++;
		return ret;
	}

	shm->opus_seq = 0;
	memcpy((void *)shm->opus_packet, packet, (size_t)ret);
	shm->opus_len = (uint32_t)ret;
	shm->opus_checksum = core1_checksum32(packet, (uint32_t)ret);
	shm->opus_encode_count++;
	shm->opus_seq = ++(*opus_seq);
	return 0;
}

static int core1_es8311_play_birthday(volatile struct pico_clip_core1_test_shared *shm)
{
	PIO i2s_pio = pio2;
	uint i2s_sm = 0;
	dma_channel_config tx_config;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	(void)core1_es8311_write(ES8311_ADC_REG16, ES8311_BIRTHDAY_ADC_GAIN);
	(void)core1_es8311_write(ES8311_DAC_REG32, ES8311_BIRTHDAY_DAC_VOLUME);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(i2s_pio, i2s_sm, true));

	shm->audio_error = 0;
	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	while (!core1_audio_stop_requested(shm)) {
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
		shm->audio_progress = CORE1_AUDIO_PROGRESS_DMA_PLAY;
		dma_channel_configure(core1_audio_dma_chan,
				      &tx_config,
				      &i2s_pio->txf[i2s_sm],
				      Happy_birsday,
				      ES8311_BIRTHDAY_SAMPLES,
				      true);

		while (dma_channel_is_busy(core1_audio_dma_chan)) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_dma_chan);
				pio_sm_clear_fifos(i2s_pio, i2s_sm);
				shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
				shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
				return 0;
			}
			core1_delay_loops(2000U);
		}

		shm->audio_play_count++;
	}

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_birthday_opus(volatile struct pico_clip_core1_test_shared *shm)
{
	PIO tx_pio = pio2;
	uint tx_sm = 0;
	dma_channel_config tx_config;
	uint32_t opus_seq = shm->opus_seq;
	bool stop = false;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	ret = core1_opus_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, tx_sm, true));

	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_OPUS;

	while (!stop) {
		uint32_t offset = 0;

		pio_sm_clear_fifos(tx_pio, tx_sm);
		dma_channel_configure(core1_audio_dma_chan,
				      &tx_config,
				      &tx_pio->txf[tx_sm],
				      Happy_birsday,
				      ES8311_BIRTHDAY_SAMPLES,
				      true);

		while (offset < ES8311_BIRTHDAY_SAMPLES && !stop) {
			uint32_t played;

			(void)core1_publish_mic_opus(
				shm, (int16_t *)&Happy_birsday[offset], &opus_seq);
			offset += PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES;

			do {
				uint32_t remaining =
					dma_channel_hw_addr(core1_audio_dma_chan)->transfer_count &
					DMA_CH0_TRANS_COUNT_COUNT_BITS;

				played = ES8311_BIRTHDAY_SAMPLES - remaining;
				shm->cpu1_heartbeat++;
				shm->audio_heartbeat++;
				core1_service_test_command(shm);
				if (core1_audio_stop_requested(shm)) {
					dma_channel_abort(core1_audio_dma_chan);
					stop = true;
					break;
				}
			} while (played < offset &&
				 dma_channel_is_busy(core1_audio_dma_chan));
		}

		while (dma_channel_is_busy(core1_audio_dma_chan) && !stop) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_dma_chan);
				stop = true;
			}
		}
		if (!stop) {
			shm->audio_play_count++;
		}
	}
	pio_sm_clear_fifos(tx_pio, tx_sm);

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_birthday_20ms(volatile struct pico_clip_core1_test_shared *shm)
{
	PIO tx_pio = pio2;
	uint tx_sm = 0;
	dma_channel_config tx_config;
	uint32_t offset = 0;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, tx_sm, true));
	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DMA_PLAY;

	while (!core1_audio_stop_requested(shm)) {
		uint32_t samples = ES8311_BIRTHDAY_SAMPLES - offset;

		if (samples > PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES) {
			samples = PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES;
		}
		ret = core1_play_pcm_frame(shm, &tx_config, &Happy_birsday[offset], samples);
		if (ret == -ECANCELED) {
			break;
		}
		offset += samples;
		if (offset >= ES8311_BIRTHDAY_SAMPLES) {
			offset = 0;
			shm->audio_play_count++;
		}
	}

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_birthday_stream(volatile struct pico_clip_core1_test_shared *shm)
{
	uint32_t offset = 0;
	uint32_t opus_seq = shm->opus_seq;
	uint32_t deadline = timer0_hw->timerawl;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_opus_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_OPUS;

	while (!core1_audio_stop_requested(shm)) {
		(void)core1_publish_mic_opus(
			shm, (int16_t *)&Happy_birsday[offset], &opus_seq);
		offset += PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES;
		if (offset >= ES8311_BIRTHDAY_SAMPLES) {
			offset = 0;
			shm->audio_play_count++;
		}

		deadline += 20000U;
		while ((int32_t)(deadline - timer0_hw->timerawl) > 0) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
				shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
				return 0;
			}
		}
	}

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_birthday_codec_loopback(
	volatile struct pico_clip_core1_test_shared *shm)
{
	PIO tx_pio = pio2;
	uint tx_sm = 0;
	dma_channel_config tx_config;
	int16_t *decoded[2] = { core1_opus_pcm_frame(), core1_opus_pcm_alt_frame() };
	uint32_t decoded_samples[2] = { 0, 0 };
	uint32_t play_index = 0;
	uint32_t offset = 0;
	uint32_t opus_seq = shm->opus_seq;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	ret = core1_opus_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, tx_sm, true));
	(void)core1_es8311_write(ES8311_DAC_REG32, ES8311_BIRTHDAY_DAC_VOLUME);
	pio_sm_clear_fifos(tx_pio, tx_sm);

	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_OPUS;

	/* Prime the first decoded buffer, then overlap DMA playback of one buffer
	 * with encode/decode into the other buffer. */
	ret = core1_publish_mic_opus(shm, (int16_t *)&Happy_birsday[offset], &opus_seq);
	if (ret == 0) {
		ret = opus_decode(core1_opus_decoder(), core1_opus_packet_tmp(),
				  (opus_int32)shm->opus_len, decoded[play_index],
				  PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, 0);
	}
	if (ret < 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}
	decoded_samples[play_index] = (uint32_t)ret;
	offset += PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES;

	while (!core1_audio_stop_requested(shm)) {
		uint32_t fill_index = play_index ^ 1U;

		dma_channel_configure(core1_audio_dma_chan, &tx_config,
				      &tx_pio->txf[tx_sm], decoded[play_index],
				      decoded_samples[play_index], true);

		if (offset >= ES8311_BIRTHDAY_SAMPLES) {
			offset = 0;
		}
		ret = core1_publish_mic_opus(
			shm, (int16_t *)&Happy_birsday[offset], &opus_seq);
		if (ret == 0) {
			ret = opus_decode(core1_opus_decoder(), core1_opus_packet_tmp(),
					  (opus_int32)shm->opus_len, decoded[fill_index],
					  PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, 0);
		}
		if (ret < 0) {
			shm->audio_error = ret;
			shm->spk_opus_dropped++;
			dma_channel_wait_for_finish_blocking(core1_audio_dma_chan);
			continue;
		}
		decoded_samples[fill_index] = (uint32_t)ret;
		offset += PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES;

		while (dma_channel_is_busy(core1_audio_dma_chan)) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_dma_chan);
				goto birthday_codec_done;
			}
		}

		shm->opus_decode_count++;
		shm->audio_play_count++;
		play_index = fill_index;
	}

birthday_codec_done:
	pio_sm_clear_fifos(tx_pio, tx_sm);
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_es8311_loopback(volatile struct pico_clip_core1_test_shared *shm)
{
	PIO tx_pio = pio2;
	PIO rx_pio = pio1;
	uint tx_sm = 0;
	uint rx_sm = 1;
	int16_t *loopback_buffer = (int16_t *)CORE1_LOOPBACK_BUFFER_ADDR;
	dma_channel_config rx_config;
	dma_channel_config tx_config;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	ret = core1_es8311_din_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	if (core1_audio_rx_dma_chan == UINT_MAX) {
		int dma_chan = dma_claim_unused_channel(false);

		if (dma_chan < 0) {
			shm->audio_error = -EBUSY;
			shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
			return -EBUSY;
		}
		core1_audio_rx_dma_chan = (uint)dma_chan;
	}

	rx_config = dma_channel_get_default_config(core1_audio_rx_dma_chan);
	channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_config, false);
	channel_config_set_write_increment(&rx_config, true);
	channel_config_set_dreq(&rx_config, pio_get_dreq(rx_pio, rx_sm, false));

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, tx_sm, true));

	(void)core1_es8311_write(ES8311_ADC_REG16, ES8311_LOOPBACK_ADC_GAIN);
	(void)core1_es8311_write(ES8311_DAC_REG32, ES8311_LOOPBACK_DAC_VOLUME);
	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_LOOPBACK;
	pio_sm_clear_fifos(rx_pio, rx_sm);
	pio_sm_clear_fifos(tx_pio, tx_sm);

	while (!core1_audio_stop_requested(shm)) {
		int16_t min_sample = INT16_MAX;
		int16_t max_sample = INT16_MIN;
		uint32_t nonzero = 0;

		dma_channel_configure(core1_audio_rx_dma_chan,
				      &rx_config,
				      loopback_buffer,
				      &rx_pio->rxf[rx_sm],
				      ES8311_LOOPBACK_SAMPLES,
				      true);

		while (dma_channel_is_busy(core1_audio_rx_dma_chan)) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_rx_dma_chan);
				pio_sm_clear_fifos(rx_pio, rx_sm);
				shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
				shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
				return 0;
			}
		}

		for (uint32_t i = 0; i < ES8311_LOOPBACK_SAMPLES; i++) {
			int16_t sample = loopback_buffer[i];

			if (sample != 0) {
				nonzero++;
			}
			if (sample < min_sample) {
				min_sample = sample;
			}
			if (sample > max_sample) {
				max_sample = sample;
			}
		}
		shm->audio_sample_min = min_sample;
		shm->audio_sample_max = max_sample;
		shm->audio_sample_nonzero = nonzero;

		dma_channel_configure(core1_audio_dma_chan,
				      &tx_config,
				      &tx_pio->txf[tx_sm],
				      loopback_buffer,
				      ES8311_LOOPBACK_SAMPLES,
				      true);

		while (dma_channel_is_busy(core1_audio_dma_chan)) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_dma_chan);
				pio_sm_clear_fifos(tx_pio, tx_sm);
				shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
				shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
				return 0;
			}
		}

		shm->audio_play_count++;
	}

	pio_sm_clear_fifos(rx_pio, rx_sm);
	pio_sm_clear_fifos(tx_pio, tx_sm);
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static int core1_es8311_opus(volatile struct pico_clip_core1_test_shared *shm,
			     bool local_loopback)
{
	PIO tx_pio = pio2;
	PIO rx_pio = pio1;
	uint tx_sm = 0;
	uint rx_sm = 1;
	dma_channel_config rx_config;
	dma_channel_config tx_config;
	int16_t *pcm[2] = { core1_opus_pcm_frame(), core1_opus_pcm_alt_frame() };
	uint32_t capture_index = 0;
	uint32_t opus_seq = shm->opus_seq;
	int ret;

	shm->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_es8311_hw_init();
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	ret = core1_es8311_din_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	if (core1_audio_rx_dma_chan == UINT_MAX) {
		int dma_chan = dma_claim_unused_channel(false);

		if (dma_chan < 0) {
			shm->audio_error = -EBUSY;
			shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
			return -EBUSY;
		}
		core1_audio_rx_dma_chan = (uint)dma_chan;
	}

	ret = core1_opus_init(shm);
	if (ret != 0) {
		shm->audio_error = ret;
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;
		return ret;
	}

	rx_config = dma_channel_get_default_config(core1_audio_rx_dma_chan);
	channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_config, false);
	channel_config_set_write_increment(&rx_config, true);
	channel_config_set_dreq(&rx_config, pio_get_dreq(rx_pio, rx_sm, false));

	tx_config = dma_channel_get_default_config(core1_audio_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, tx_sm, true));

	(void)core1_es8311_write(ES8311_ADC_REG16, ES8311_LOOPBACK_ADC_GAIN);
	(void)core1_es8311_write(ES8311_DAC_REG32, ES8311_LOOPBACK_DAC_VOLUME);
	shm->audio_error = 0;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_OPUS;

	/* Keep the I2S receiver running continuously.  Start filling the first
	 * buffer once, then re-arm DMA onto the other buffer before encoding the
	 * completed one.  In particular, do not clear the RX FIFO at every Opus
	 * frame: that loses I2S word alignment and was the source of noisy audio.
	 */
	pio_sm_clear_fifos(rx_pio, rx_sm);
	dma_channel_configure(core1_audio_rx_dma_chan, &rx_config,
			      pcm[capture_index], &rx_pio->rxf[rx_sm],
			      PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, true);

	while (!core1_audio_stop_requested(shm)) {
		int16_t *completed = pcm[capture_index];
		int16_t min_sample = INT16_MAX;
		int16_t max_sample = INT16_MIN;
		uint32_t nonzero = 0;

		while (dma_channel_is_busy(core1_audio_rx_dma_chan)) {
			shm->cpu1_heartbeat++;
			shm->audio_heartbeat++;
			core1_service_test_command(shm);
			if (core1_audio_stop_requested(shm)) {
				dma_channel_abort(core1_audio_rx_dma_chan);
				goto opus_done;
			}
		}

		/* Arm the next 20 ms capture before doing any CPU-heavy Opus work. */
		capture_index ^= 1U;
		dma_channel_configure(core1_audio_rx_dma_chan, &rx_config,
				      pcm[capture_index], &rx_pio->rxf[rx_sm],
				      PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, true);

		for (uint32_t i = 0; i < PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES; i++) {
			int16_t sample = completed[i];

			nonzero += sample != 0;
			if (sample < min_sample) {
				min_sample = sample;
			}
			if (sample > max_sample) {
				max_sample = sample;
			}
		}
		shm->audio_sample_min = min_sample;
		shm->audio_sample_max = max_sample;
		shm->audio_sample_nonzero = nonzero;

		ret = core1_publish_mic_opus(shm, completed, &opus_seq);
		if (ret == 0 && local_loopback) {
			ret = opus_decode(core1_opus_decoder(), core1_opus_packet_tmp(),
					  (opus_int32)shm->opus_len,
					  core1_opus_spk_frame(),
					  PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES, 0);
			if (ret < 0) {
				shm->audio_error = ret;
				shm->spk_opus_dropped++;
			} else if (core1_play_pcm_frame(shm, &tx_config,
							 core1_opus_spk_frame(),
							 (uint32_t)ret) == 0) {
				shm->opus_decode_count++;
				shm->audio_play_count++;
			}
		} else if (!local_loopback) {
			(void)core1_decode_speaker_packet(shm, &tx_config);
		}
		shm->cpu1_heartbeat++;
		shm->audio_heartbeat++;
		core1_service_test_command(shm);
	}

opus_done:
	if (core1_audio_rx_dma_chan != UINT_MAX) {
		dma_channel_abort(core1_audio_rx_dma_chan);
	}
	if (core1_audio_dma_chan != UINT_MAX) {
		dma_channel_abort(core1_audio_dma_chan);
	}
	pio_sm_clear_fifos(rx_pio, rx_sm);
	pio_sm_clear_fifos(tx_pio, tx_sm);
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	shm->audio_progress = CORE1_AUDIO_PROGRESS_DONE;
	return 0;
}

static void core1_audio_handle_command(volatile struct pico_clip_core1_test_shared *shm)
{
	uint32_t seq = shm->cpu0_audio_cmd_seq;
	uint32_t cmd = shm->cpu0_audio_cmd;

	if (seq == shm->cpu1_audio_ack_seq) {
		return;
	}

	shm->cpu1_audio_ack_seq = seq;
	if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY) {
		(void)core1_es8311_play_birthday(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_LOOPBACK) {
		(void)core1_es8311_loopback(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_OPUS) {
		(void)core1_es8311_opus(shm, false);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_MIC_OPUS_LOOPBACK) {
		(void)core1_es8311_opus(shm, true);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_OPUS) {
		(void)core1_birthday_opus(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_20MS) {
		(void)core1_birthday_20ms(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_STREAM) {
		(void)core1_birthday_stream(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_CODEC_LOOPBACK) {
		(void)core1_birthday_codec_loopback(shm);
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_STOP) {
		if (core1_audio_dma_chan != UINT_MAX &&
		    dma_channel_is_busy(core1_audio_dma_chan)) {
			dma_channel_abort(core1_audio_dma_chan);
		}
		if (core1_audio_rx_dma_chan != UINT_MAX &&
		    dma_channel_is_busy(core1_audio_rx_dma_chan)) {
			dma_channel_abort(core1_audio_rx_dma_chan);
		}
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	} else {
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_IDLE;
	}
}

static void __attribute__((noreturn, noinline)) core1_bare_worker(void)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	shm->magic = PICO_CLIP_CORE1_TEST_MAGIC;
	shm->version = PICO_CLIP_CORE1_TEST_VERSION;
	shm->cpu1_boot_count++;
	shm->audio_state = PICO_CLIP_CORE1_AUDIO_IDLE;

	while (true) {
		shm->cpu1_heartbeat++;
		core1_service_test_command(shm);
		core1_audio_handle_command(shm);

		for (volatile uint32_t i = 0; i < 10000U; i++) {
		}
	}
}

static int core1_start_bare_impl(const struct shell *sh)
{
	uint32_t *vector_table = (uint32_t *)CPU1_SRAM_ADDR;
	uint32_t sp = CPU1_SRAM_ADDR + CPU1_SRAM_SIZE - 8U;
	uint32_t pc = (uint32_t)core1_bare_worker | 1U;
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	memset((void *)shm, 0, sizeof(*shm));
	vector_table[0] = sp;
	vector_table[1] = pc;
	__DSB();
	__ISB();

	if (sh != NULL) {
		shell_print(sh, "core1 bare: vt=%p sp=0x%08x pc=0x%08x worker=%p",
			    vector_table, sp, pc, core1_bare_worker);
	}

	int ret = core1_reset_with_timeout(sh);

	if (ret != 0) {
		return ret;
	}

	ret = core1_boot_no_final_wait(sh, (uint32_t)vector_table, sp, pc);
	if (ret != 0) {
		return ret;
	}

	for (int i = 0; i < 20; i++) {
		if (shm->magic == PICO_CLIP_CORE1_TEST_MAGIC &&
		    shm->version == PICO_CLIP_CORE1_TEST_VERSION) {
			if (sh != NULL) {
				shell_print(sh, "core1 bare: ready boot=%u heartbeat=%u",
					    shm->cpu1_boot_count, shm->cpu1_heartbeat);
			}
			return 0;
		}

		k_sleep(K_MSEC(10));
	}

	if (sh != NULL) {
		shell_error(sh, "core1 bare: not ready magic=0x%08x version=%u heartbeat=%u",
			    shm->magic, shm->version, shm->cpu1_heartbeat);
	}
	return -ETIMEDOUT;
}

static int core1_bare_autostart(void)
{
	return core1_start_bare_impl(NULL);
}

SYS_INIT(core1_bare_autostart, APPLICATION, 90);

static int core1_start_bare(const struct shell *sh)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	if (shm->magic == PICO_CLIP_CORE1_TEST_MAGIC &&
	    shm->version == PICO_CLIP_CORE1_TEST_VERSION) {
		shell_print(sh, "core1 bare already running: boot=%u heartbeat=%u",
			    shm->cpu1_boot_count, shm->cpu1_heartbeat);
		return 0;
	}

	return core1_start_bare_impl(sh);
}

static const char *core1_audio_state_name(uint32_t state)
{
	switch (state) {
	case PICO_CLIP_CORE1_AUDIO_IDLE:
		return "idle";
	case PICO_CLIP_CORE1_AUDIO_INIT:
		return "init";
	case PICO_CLIP_CORE1_AUDIO_PLAYING:
		return "playing";
	case PICO_CLIP_CORE1_AUDIO_DONE:
		return "done";
	case PICO_CLIP_CORE1_AUDIO_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static int core1_audio_send(const struct shell *sh, uint32_t cmd)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();
	uint32_t seq;

	if (shm->magic != PICO_CLIP_CORE1_TEST_MAGIC ||
	    shm->version != PICO_CLIP_CORE1_TEST_VERSION) {
		shell_error(sh, "core1 test not ready: magic=0x%08x version=%u",
			    shm->magic, shm->version);
		return -ENODEV;
	}

	seq = shm->cpu0_audio_cmd_seq + 1U;
	shm->cpu0_audio_cmd = cmd;
	shm->cpu0_audio_cmd_seq = seq;
	__SEV();

	for (int i = 0; i < 50 && shm->cpu1_audio_ack_seq != seq; i++) {
		k_sleep(K_MSEC(10));
	}

	if (shm->cpu1_audio_ack_seq != seq) {
		shell_error(sh, "core1 audio command timeout: cmd=%u seq=%u ack=%u",
			    cmd, seq, shm->cpu1_audio_ack_seq);
		return -ETIMEDOUT;
	}

	shell_print(sh, "core1 audio cmd=%u ack=%u state=%s err=%d play_count=%u",
		    cmd, shm->cpu1_audio_ack_seq,
		    core1_audio_state_name(shm->audio_state),
		    shm->audio_error, shm->audio_play_count);
	return 0;
}

static int cmd_core1_audio_test(const struct shell *sh, size_t argc, char **argv)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	if (argc < 2) {
		shell_print(sh,
			    "Usage: core1_audio_test start|status|birthday|loopback|opus|stop");
		return -EINVAL;
	}

	if (strcmp(argv[1], "start") == 0 ||
	    strcmp(argv[1], "start_bare") == 0) {
		return core1_start_bare(sh);
	}

	if (strcmp(argv[1], "birthday") == 0 ||
	    strcmp(argv[1], "es8311_birthday") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY);
	}

	if (strcmp(argv[1], "loopback") == 0 ||
	    strcmp(argv[1], "loop_test") == 0 ||
	    strcmp(argv[1], "loop") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_LOOPBACK);
	}

	if (strcmp(argv[1], "opus") == 0 ||
	    strcmp(argv[1], "voice") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_OPUS);
	}

	if (strcmp(argv[1], "mic_opus_loopback") == 0 ||
	    strcmp(argv[1], "opus_loopback") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_MIC_OPUS_LOOPBACK);
	}

	if (strcmp(argv[1], "birthday_opus") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_OPUS);
	}

	if (strcmp(argv[1], "birthday_20ms") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_20MS);
	}

	if (strcmp(argv[1], "birthday_stream") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_STREAM);
	}

	if (strcmp(argv[1], "birthday_codec_loopback") == 0 ||
	    strcmp(argv[1], "birthday_loopback") == 0) {
		return core1_audio_send(
			sh, PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY_CODEC_LOOPBACK);
	}

	if (strcmp(argv[1], "stop") == 0 ||
	    strcmp(argv[1], "es8311_stop") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_STOP);
	}

	if (strcmp(argv[1], "status") != 0) {
		shell_print(sh,
			    "Usage: core1_audio_test start|status|birthday|loopback|opus|stop");
		return -EINVAL;
	}

	if (shm->magic != PICO_CLIP_CORE1_TEST_MAGIC ||
	    shm->version != PICO_CLIP_CORE1_TEST_VERSION) {
		shell_error(sh, "core1 test not ready: magic=0x%08x version=%u",
			    shm->magic, shm->version);
		return -ENODEV;
	}

	uint32_t seq = shm->cpu0_cmd_seq + 1U;
	uint32_t heartbeat0 = shm->cpu1_heartbeat;

	shm->cpu0_cmd_value = seq * 10U;
	shm->cpu0_cmd_seq = seq;
	__SEV();

	for (int i = 0; i < 20 && shm->cpu1_ack_seq != seq; i++) {
		k_sleep(K_MSEC(10));
	}

	uint32_t heartbeat1 = shm->cpu1_heartbeat;

	shell_print(sh,
		    "core1: boot=%u heartbeat=%u->%u %s cmd=%u ack=%u response=%u",
		    shm->cpu1_boot_count, heartbeat0, heartbeat1,
		    heartbeat0 == heartbeat1 ? "stalled" : "running",
		    seq, shm->cpu1_ack_seq, shm->cpu1_response_value);
	shell_print(sh,
		    "core1 audio: state=%s err=%d progress=%u heartbeat=%u play_count=%u cmd=%u ack=%u",
		    core1_audio_state_name(shm->audio_state), shm->audio_error,
		    shm->audio_progress, shm->audio_heartbeat, shm->audio_play_count,
		    shm->cpu0_audio_cmd_seq, shm->cpu1_audio_ack_seq);
	shell_print(sh, "core1 audio samples: min=%d max=%d nonzero=%u",
		    shm->audio_sample_min, shm->audio_sample_max,
		    shm->audio_sample_nonzero);
	shell_print(sh,
		    "core1 opus: flags=0x%x seq=%u len=%u checksum=0x%08x enc=%u dec=%u dropped=%u bitrate=%u enc_size=%u dec_size=%u",
		    shm->audio_flags, shm->opus_seq, shm->opus_len, shm->opus_checksum,
		    shm->opus_encode_count, shm->opus_decode_count, shm->opus_dropped,
		    shm->opus_bitrate, shm->opus_encoder_size, shm->opus_decoder_size);
	shell_print(sh, "core1 speaker opus: pending=%u dropped=%u",
		    shm->spk_opus_write_seq - shm->spk_opus_read_seq,
		    shm->spk_opus_dropped);
	return 0;
}

SHELL_CMD_REGISTER(core1_audio_test, NULL, "Core1 bare-metal audio test", cmd_core1_audio_test);
SHELL_CMD_REGISTER(core1_test, NULL, "Alias for core1_audio_test", cmd_core1_audio_test);
