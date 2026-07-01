// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/regs/clocks.h>

#include "audio_pio.pio.h"

#define I2S_NODE DT_NODELABEL(pio1_i2s)

#define ES8311_ADDR 0x18
#define ES8311_I2C_RETRIES 5
#define ES8311_RESET_REG00 0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09 0x09
#define ES8311_SDPOUT_REG0A 0x0a
#define ES8311_SYSTEM_REG0B 0x0b
#define ES8311_SYSTEM_REG0C 0x0c
#define ES8311_SYSTEM_REG0D 0x0d
#define ES8311_SYSTEM_REG0E 0x0e
#define ES8311_SYSTEM_REG10 0x10
#define ES8311_SYSTEM_REG11 0x11
#define ES8311_SYSTEM_REG12 0x12
#define ES8311_SYSTEM_REG13 0x13
#define ES8311_SYSTEM_REG14 0x14
#define ES8311_ADC_REG15 0x15
#define ES8311_ADC_REG16 0x16
#define ES8311_ADC_REG17 0x17
#define ES8311_ADC_REG1B 0x1b
#define ES8311_ADC_REG1C 0x1c
#define ES8311_DAC_REG31 0x31
#define ES8311_DAC_REG32 0x32
#define ES8311_DAC_REG37 0x37
#define ES8311_GPIO_REG44 0x44
#define ES8311_GP_REG45 0x45
#define ES8311_CHD1_REGFD 0xfd
#define ES8311_CHD2_REGFE 0xfe
#define ES8311_CHVER_REGFF 0xff

#define SAMPLE_RATE 24000U
#define MCLK_HZ (SAMPLE_RATE * 256U)
#define PA_CTRL_GPIO 0U
#define DOUT_GPIO 1U
#define DIN_GPIO 2U
#define MCLK_GPIO 3U
#define BCLK_GPIO 4U
#define LRCLK_GPIO 5U
#define BAT_EN_GPIO 28U
#define HAPPY_BIRTHDAY_SAMPLES 124800U
#define LOOPBACK_SAMPLES 4096U
#define PLAYER_STACK_SIZE 2048

static const struct device *const es8311_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

extern const int16_t Happy_birsday[];

static PIO i2s_pio = pio2;
static PIO mclk_pio = pio1;
static PIO din_pio = pio1;
static uint i2s_sm = 0;
static uint mclk_sm = 2;
static uint din_sm = 1;
static uint tx_dma_channel;
static dma_channel_config tx_dma_config;
static int16_t loopback_buffer[LOOPBACK_SAMPLES];
static bool mclk_started;
static bool dma_ready;
static bool codec_i2c_ready;
static bool codec_ready;
static bool i2s_ready;
static bool din_ready;
static volatile bool player_running;
static volatile bool loopback_running;

K_THREAD_STACK_DEFINE(player_stack, PLAYER_STACK_SIZE);
static struct k_thread player_thread_data;
static k_tid_t player_thread_id;

static int es8311_write(uint8_t reg, uint8_t val)
{
	uint8_t buf[] = {reg, val};
	int ret = 0;

	for (int i = 0; i < ES8311_I2C_RETRIES; i++) {
		ret = i2c_write(es8311_i2c_dev, buf, sizeof(buf), ES8311_ADDR);
		if (ret == 0) {
			k_busy_wait(500);
			return 0;
		}
		k_msleep(2);
	}

	return ret < 0 ? ret : -EIO;
}

static int es8311_read(uint8_t reg, uint8_t *val)
{
	int ret = 0;

	for (int i = 0; i < ES8311_I2C_RETRIES; i++) {
		ret = i2c_write_read(es8311_i2c_dev, ES8311_ADDR, &reg, 1, val, 1);
		if (ret == 0) {
			k_busy_wait(500);
			return 0;
		}
		k_msleep(2);
	}

	return ret < 0 ? ret : -EIO;
}

static int es8311_update(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t old_val;
	int ret = es8311_read(reg, &old_val);

	if (ret < 0) {
		return ret;
	}
	return es8311_write(reg, (old_val & ~mask) | (val & mask));
}

static int es8311_i2c_start(void)
{
	if (codec_i2c_ready) {
		return 0;
	}

	if (!device_is_ready(es8311_i2c_dev)) {
		printk("i2c1 is not ready\n");
		return -ENODEV;
	}
	codec_i2c_ready = true;
	return 0;
}

static int es8311_init(void)
{
	uint8_t id1 = 0;
	uint8_t id2 = 0;
	uint8_t ver = 0;
	uint8_t reg00;
	uint8_t reg02;
	uint8_t reg06;
	int ret;

	ret = es8311_i2c_start();
	if (ret < 0) {
		return ret;
	}
	ret = es8311_read(ES8311_CHD1_REGFD, &id1);
	if (ret < 0) {
		return ret;
	}
	(void)es8311_read(ES8311_CHD2_REGFE, &id2);
	(void)es8311_read(ES8311_CHVER_REGFF, &ver);

	ret = es8311_write(ES8311_RESET_REG00, 0x1f);
	k_msleep(20);
	ret |= es8311_write(ES8311_RESET_REG00, 0x00);
	ret |= es8311_write(ES8311_RESET_REG00, 0x80);

	ret |= es8311_write(ES8311_CLK_MANAGER_REG01, 0x3f);
	ret |= es8311_read(ES8311_CLK_MANAGER_REG06, &reg06);
	reg06 &= ~BIT(5);
	reg06 |= 0x03;
	ret |= es8311_write(ES8311_CLK_MANAGER_REG06, reg06);

	ret |= es8311_read(ES8311_CLK_MANAGER_REG02, &reg02);
	reg02 &= 0x07;
	ret |= es8311_write(ES8311_CLK_MANAGER_REG02, reg02);
	ret |= es8311_write(ES8311_CLK_MANAGER_REG03, 0x10);
	ret |= es8311_write(ES8311_CLK_MANAGER_REG04, 0x10);
	ret |= es8311_write(ES8311_CLK_MANAGER_REG05, 0x00);
	ret |= es8311_read(ES8311_CLK_MANAGER_REG06, &reg06);
	reg06 &= 0xe0;
	reg06 |= 0x07;
	ret |= es8311_write(ES8311_CLK_MANAGER_REG06, reg06);
	ret |= es8311_update(ES8311_CLK_MANAGER_REG07, 0x3f, 0x00);
	ret |= es8311_write(ES8311_CLK_MANAGER_REG08, 0xff);

	ret |= es8311_read(ES8311_RESET_REG00, &reg00);
	reg00 |= 0x40;
	ret |= es8311_write(ES8311_RESET_REG00, reg00);
	ret |= es8311_write(ES8311_SDPIN_REG09, 0x0c);
	ret |= es8311_write(ES8311_SDPOUT_REG0A, 0x0c);

	ret |= es8311_write(ES8311_SYSTEM_REG0D, 0x01);
	ret |= es8311_write(ES8311_SYSTEM_REG0E, 0x02);
	ret |= es8311_write(ES8311_SYSTEM_REG12, 0x00);
	ret |= es8311_write(ES8311_SYSTEM_REG13, 0x10);
	ret |= es8311_write(ES8311_ADC_REG1C, 0x6a);
	ret |= es8311_write(ES8311_DAC_REG37, 0x08);
	ret |= es8311_write(ES8311_ADC_REG17, 0xff);
	ret |= es8311_write(ES8311_SYSTEM_REG14, 0x1a);
	ret |= es8311_write(ES8311_ADC_REG16, 0x03);
	ret |= es8311_write(ES8311_DAC_REG32, 0x98);
	ret |= es8311_update(ES8311_DAC_REG31, 0x60, 0x00);

	if (ret < 0) {
		return ret;
	}

	printk("ES8311 ready: id=%02x/%02x/%02x sample_rate=%u mclk=%u\n",
	       id1, id2, ver, SAMPLE_RATE, MCLK_HZ);
	codec_ready = true;
	return 0;
}

static float mclk_clock_div(void)
{
	return (float)clock_get_hz(clk_sys) / (float)(MCLK_HZ * mclk_pio_program.length);
}

static int i2s_start(void)
{
	uint32_t offset;

	if (i2s_ready) {
		return 0;
	}

	pio_sm_claim(i2s_pio, i2s_sm);
	offset = pio_add_program(i2s_pio, &audio_pio_program);
	audio_pio_program_init(i2s_pio, i2s_sm, offset, DOUT_GPIO, LRCLK_GPIO);
	pio_sm_set_clkdiv(i2s_pio, i2s_sm, 1.0f);
	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	pio_sm_restart(i2s_pio, i2s_sm);
	pio_sm_set_enabled(i2s_pio, i2s_sm, true);
	i2s_ready = true;

	printk("ES8311 I2S ready: mclk=GP%u dout=GP%u din=GP%u bclk=GP%u lrclk=GP%u\n",
	       MCLK_GPIO, DOUT_GPIO, DIN_GPIO, BCLK_GPIO, LRCLK_GPIO);
	return 0;
}

static int din_start(void)
{
	uint32_t offset;

	if (din_ready) {
		return 0;
	}

	pio_sm_claim(din_pio, din_sm);
	offset = pio_add_program(din_pio, &read_pio_program);
	read_pio_program_init(din_pio, din_sm, offset, DIN_GPIO, LRCLK_GPIO);
	pio_sm_set_clkdiv(din_pio, din_sm, 1.0f);
	pio_sm_clear_fifos(din_pio, din_sm);
	pio_sm_restart(din_pio, din_sm);
	pio_sm_set_enabled(din_pio, din_sm, true);
	din_ready = true;
	return 0;
}

static void mclk_start(void)
{
	uint32_t offset;

	if (mclk_started) {
		return;
	}

	pio_sm_claim(mclk_pio, mclk_sm);
	offset = pio_add_program(mclk_pio, &mclk_pio_program);
	mclk_pio_program_init(mclk_pio, mclk_sm, offset, MCLK_GPIO);
	pio_sm_set_clkdiv(mclk_pio, mclk_sm, mclk_clock_div());
	pio_sm_clear_fifos(mclk_pio, mclk_sm);
	pio_sm_restart(mclk_pio, mclk_sm);
	pio_sm_set_enabled(mclk_pio, mclk_sm, true);
	mclk_started = true;
}

static int es8311_prepare(void)
{
	int ret;

	gpio_init(PA_CTRL_GPIO);
	gpio_set_dir(PA_CTRL_GPIO, GPIO_OUT);
	gpio_put(PA_CTRL_GPIO, 1);

	gpio_init(BAT_EN_GPIO);
	gpio_set_dir(BAT_EN_GPIO, GPIO_OUT);
	gpio_put(BAT_EN_GPIO, 1);

	if (!dma_ready) {
		tx_dma_channel = dma_claim_unused_channel(true);
		tx_dma_config = dma_channel_get_default_config(tx_dma_channel);
		channel_config_set_transfer_data_size(&tx_dma_config, DMA_SIZE_16);
		channel_config_set_read_increment(&tx_dma_config, true);
		channel_config_set_write_increment(&tx_dma_config, false);
		dma_ready = true;
	}

	mclk_start();
	k_msleep(20);

	if (!codec_ready) {
		ret = es8311_init();
		if (ret < 0) {
			return ret;
		}
	}

	return i2s_start();
}

static void birthday_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	channel_config_set_dreq(&tx_dma_config, pio_get_dreq(i2s_pio, i2s_sm, true));

	while (player_running) {
		dma_channel_configure(tx_dma_channel,
				      &tx_dma_config,
				      &i2s_pio->txf[i2s_sm],
				      Happy_birsday,
				      HAPPY_BIRTHDAY_SAMPLES,
				      true);
		dma_channel_wait_for_finish_blocking(tx_dma_channel);
	}
	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	player_running = false;
	player_thread_id = NULL;
}

static void loopback_worker(void *a, void *b, void *c)
{
	uint rx_dma_chan;
	uint tx_dma_chan;
	dma_channel_config rx_config;
	dma_channel_config tx_config;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	rx_dma_chan = dma_claim_unused_channel(true);
	rx_config = dma_channel_get_default_config(rx_dma_chan);
	channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_config, false);
	channel_config_set_write_increment(&rx_config, true);
	channel_config_set_dreq(&rx_config, pio_get_dreq(din_pio, din_sm, false));

	tx_dma_chan = dma_claim_unused_channel(true);
	tx_config = dma_channel_get_default_config(tx_dma_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config, pio_get_dreq(i2s_pio, i2s_sm, true));

	while (loopback_running) {
		dma_channel_configure(rx_dma_chan,
				      &rx_config,
				      loopback_buffer,
				      &din_pio->rxf[din_sm],
				      LOOPBACK_SAMPLES,
				      true);
		dma_channel_wait_for_finish_blocking(rx_dma_chan);
		if (!loopback_running) {
			break;
		}

		dma_channel_configure(tx_dma_chan,
				      &tx_config,
				      &i2s_pio->txf[i2s_sm],
				      loopback_buffer,
				      LOOPBACK_SAMPLES,
				      true);
		dma_channel_wait_for_finish_blocking(tx_dma_chan);
	}

	dma_channel_abort(rx_dma_chan);
	dma_channel_abort(tx_dma_chan);
	dma_channel_unclaim(rx_dma_chan);
	dma_channel_unclaim(tx_dma_chan);
	pio_sm_clear_fifos(din_pio, din_sm);
	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	loopback_running = false;
	player_thread_id = NULL;
}

static int cmd_es8311_init(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = es8311_prepare();
	if (ret < 0) {
		shell_error(sh, "ES8311 init failed: %d", ret);
		return ret;
	}

	shell_print(sh, "ES8311 ready");
	return 0;
}

static int cmd_es8311_birthday(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (player_running) {
		shell_print(sh, "birthday already playing");
		return 0;
	}
	if (loopback_running) {
		shell_error(sh, "loopback already running");
		return -EBUSY;
	}

	ret = es8311_prepare();
	if (ret < 0) {
		shell_error(sh, "ES8311 prepare failed: %d", ret);
		return ret;
	}

	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	player_running = true;
	player_thread_id = k_thread_create(&player_thread_data, player_stack,
					   K_THREAD_STACK_SIZEOF(player_stack),
					   birthday_worker, NULL, NULL, NULL,
					   K_LOWEST_APPLICATION_THREAD_PRIO, 0,
					   K_NO_WAIT);
	shell_print(sh, "playing Happy Birthday");
	return 0;
}

static int cmd_es8311_loop(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (loopback_running) {
		shell_print(sh, "loopback already running");
		return 0;
	}
	if (player_running) {
		shell_error(sh, "birthday already playing");
		return -EBUSY;
	}

	ret = es8311_prepare();
	if (ret < 0) {
		shell_error(sh, "ES8311 prepare failed: %d", ret);
		return ret;
	}
	ret = din_start();
	if (ret < 0) {
		shell_error(sh, "ES8311 DIN prepare failed: %d", ret);
		return ret;
	}

	pio_sm_clear_fifos(din_pio, din_sm);
	pio_sm_clear_fifos(i2s_pio, i2s_sm);
	loopback_running = true;
	player_thread_id = k_thread_create(&player_thread_data, player_stack,
					   K_THREAD_STACK_SIZEOF(player_stack),
					   loopback_worker, NULL, NULL, NULL,
					   K_LOWEST_APPLICATION_THREAD_PRIO, 0,
					   K_NO_WAIT);
	shell_print(sh, "ES8311 loopback running");
	return 0;
}

static int cmd_es8311_micstat(const struct shell *sh, size_t argc, char **argv)
{
	uint rx_dma_chan;
	dma_channel_config rx_config;
	int16_t min_sample = INT16_MAX;
	int16_t max_sample = INT16_MIN;
	uint32_t nonzero = 0;
	uint32_t changed = 0;
	uint32_t timeout_ms = 300;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (player_running || loopback_running) {
		shell_error(sh, "stop playback/loopback first");
		return -EBUSY;
	}

	ret = es8311_prepare();
	if (ret < 0) {
		shell_error(sh, "ES8311 prepare failed: %d", ret);
		return ret;
	}
	ret = din_start();
	if (ret < 0) {
		shell_error(sh, "ES8311 DIN prepare failed: %d", ret);
		return ret;
	}

	memset(loopback_buffer, 0, sizeof(loopback_buffer));
	pio_sm_clear_fifos(din_pio, din_sm);

	rx_dma_chan = dma_claim_unused_channel(true);
	rx_config = dma_channel_get_default_config(rx_dma_chan);
	channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_config, false);
	channel_config_set_write_increment(&rx_config, true);
	channel_config_set_dreq(&rx_config, pio_get_dreq(din_pio, din_sm, false));

	dma_channel_configure(rx_dma_chan,
			      &rx_config,
			      loopback_buffer,
			      &din_pio->rxf[din_sm],
			      LOOPBACK_SAMPLES,
			      true);

	while (dma_channel_is_busy(rx_dma_chan) && timeout_ms > 0) {
		k_msleep(1);
		timeout_ms--;
	}

	if (dma_channel_is_busy(rx_dma_chan)) {
		dma_channel_abort(rx_dma_chan);
		dma_channel_unclaim(rx_dma_chan);
		shell_error(sh, "mic RX timeout: no DIN samples within 300ms");
		return -ETIMEDOUT;
	}

	dma_channel_unclaim(rx_dma_chan);

	for (size_t i = 0; i < LOOPBACK_SAMPLES; i++) {
		int16_t sample = loopback_buffer[i];

		if (sample != 0) {
			nonzero++;
		}
		if (i > 0 && sample != loopback_buffer[i - 1]) {
			changed++;
		}
		if (sample < min_sample) {
			min_sample = sample;
		}
		if (sample > max_sample) {
			max_sample = sample;
		}
	}

	shell_print(sh, "mic samples=%u nonzero=%u changed=%u min=%d max=%d",
		    LOOPBACK_SAMPLES, nonzero, changed, min_sample, max_sample);
	return 0;
}

static int cmd_es8311_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	player_running = false;
	loopback_running = false;
	if (dma_ready) {
		dma_channel_abort(tx_dma_channel);
	}
	k_msleep(5);
	if (din_ready) {
		pio_sm_clear_fifos(din_pio, din_sm);
	}
	if (i2s_ready) {
		pio_sm_clear_fifos(i2s_pio, i2s_sm);
	}
	shell_print(sh, "ES8311 audio stopped");
	return 0;
}

static int cmd_es8311_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "mclk=%s codec=%s i2s=%s din=%s playback=%s loopback=%s samples=%u loop_samples=%u",
		    mclk_started ? "on" : "off",
		    codec_ready ? "ready" : "off",
		    i2s_ready ? "ready" : "off",
		    din_ready ? "ready" : "off",
		    player_running ? "running" : "stopped",
		    loopback_running ? "running" : "stopped",
		    HAPPY_BIRTHDAY_SAMPLES,
		    LOOPBACK_SAMPLES);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(es8311_cmds,
	SHELL_CMD(birthday, NULL, "Play Happy Birthday", cmd_es8311_birthday),
	SHELL_CMD(init, NULL, "Initialize ES8311 audio", cmd_es8311_init),
	SHELL_CMD(loop, NULL, "Run ES8311 loopback test", cmd_es8311_loop),
	SHELL_CMD(micstat, NULL, "Capture ES8311 mic stats", cmd_es8311_micstat),
	SHELL_CMD(status, NULL, "Show ES8311 audio state", cmd_es8311_status),
	SHELL_CMD(stop, NULL, "Stop ES8311 playback", cmd_es8311_stop),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(es8311, &es8311_cmds, "ES8311 audio", NULL);
