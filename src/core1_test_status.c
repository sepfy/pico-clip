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
#define ES8311_SDA_GPIO 6U
#define ES8311_SCL_GPIO 7U
#define ES8311_MCLK_GPIO 3U
#define ES8311_LRCLK_GPIO 5U
#define ES8311_BAT_EN_GPIO 28U
#define ES8311_BIRTHDAY_SAMPLES 124800U
#define ES8311_BIRTHDAY_DAC_VOLUME 0x98U
#define ES8311_BIRTHDAY_ADC_GAIN 0x03U

#define CORE1_AUDIO_PROGRESS_GPIO 1U
#define CORE1_AUDIO_PROGRESS_I2C_PINS 2U
#define CORE1_AUDIO_PROGRESS_CODEC_INIT 3U
#define CORE1_AUDIO_PROGRESS_MCLK_PIO 4U
#define CORE1_AUDIO_PROGRESS_I2S_PIO 5U
#define CORE1_AUDIO_PROGRESS_DMA_CLAIM 6U
#define CORE1_AUDIO_PROGRESS_READY 7U
#define CORE1_AUDIO_PROGRESS_DMA_PLAY 8U
#define CORE1_AUDIO_PROGRESS_DONE 9U

extern const int16_t Happy_birsday[];

static bool core1_audio_ready;
static bool core1_audio_pio_ready;
static uint core1_audio_dma_chan = UINT_MAX;

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
		core1_gpio_set_func_pio(ES8311_LRCLK_GPIO, i2s_pio);
		core1_gpio_pad_init(ES8311_DOUT_GPIO, false);
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
	while (!core1_audio_stop_requested(shm)) {
		shm->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
		shm->audio_progress = CORE1_AUDIO_PROGRESS_DMA_PLAY;
		pio_sm_clear_fifos(i2s_pio, i2s_sm);
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

		pio_sm_clear_fifos(i2s_pio, i2s_sm);
		shm->audio_play_count++;
	}

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
	} else if (cmd == PICO_CLIP_CORE1_AUDIO_CMD_STOP) {
		if (core1_audio_dma_chan != UINT_MAX &&
		    dma_channel_is_busy(core1_audio_dma_chan)) {
			dma_channel_abort(core1_audio_dma_chan);
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
			    "Usage: core1_audio_test start|status|birthday|stop");
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

	if (strcmp(argv[1], "stop") == 0 ||
	    strcmp(argv[1], "es8311_stop") == 0) {
		return core1_audio_send(sh, PICO_CLIP_CORE1_AUDIO_CMD_STOP);
	}

	if (strcmp(argv[1], "status") != 0) {
		shell_print(sh,
			    "Usage: core1_audio_test start|status|birthday|stop");
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
	return 0;
}

SHELL_CMD_REGISTER(core1_audio_test, NULL, "Core1 bare-metal audio test", cmd_core1_audio_test);
SHELL_CMD_REGISTER(core1_test, NULL, "Alias for core1_audio_test", cmd_core1_audio_test);
