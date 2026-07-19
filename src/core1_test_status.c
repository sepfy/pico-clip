// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <hardware/address_mapped.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>

#include "core1_audio.h"
#include "pico_clip_core1_test_shared.h"

#define CPU1_SRAM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CPU1_SRAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CORE1_FIFO_TIMEOUT_LOOPS 100000U
#define CORE1_VECTOR_BYTES 0x100U

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

static int core1_reset(void)
{
	uint32_t reset_token;

	core1_fifo_drain();
	sio_hw->fifo_st = 0xffU;
	hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
	for (uint32_t i = 0; i < CORE1_FIFO_TIMEOUT_LOOPS; i++) {
		if ((psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS) != 0U) {
			break;
		}
		if (i == CORE1_FIFO_TIMEOUT_LOOPS - 1U) {
			return -ETIMEDOUT;
		}
	}

	hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
	if (core1_fifo_pop(&reset_token) != 0) {
		return -ETIMEDOUT;
	}
	return reset_token == 0U ? 0 : -EIO;
}

static int core1_boot(uint32_t vector_table, uint32_t sp, uint32_t pc)
{
	const uint32_t commands[] = { 0U, 0U, 1U, vector_table, sp, pc };

	for (uint32_t i = 0; i < ARRAY_SIZE(commands); i++) {
		uint32_t response;

		if (commands[i] == 0U) {
			core1_fifo_drain();
			sio_hw->fifo_st = 0xffU;
			__SEV();
		}
		if (core1_fifo_put(commands[i]) != 0) {
			return -ETIMEDOUT;
		}
		if (i == ARRAY_SIZE(commands) - 1U) {
			return 0;
		}
		if (core1_fifo_pop(&response) != 0 || response != commands[i]) {
			return -EIO;
		}
	}
	return 0;
}

static void __attribute__((noreturn, noinline)) core1_audio_worker(void)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	int ret;

	shared->magic = PICO_CLIP_CORE1_TEST_MAGIC;
	shared->version = PICO_CLIP_CORE1_TEST_VERSION;
	shared->cpu1_boot_count++;
	shared->audio_state = PICO_CLIP_CORE1_AUDIO_INIT;
	ret = core1_audio_run_opus_stream();
	shared->audio_error = ret;
	shared->audio_state = PICO_CLIP_CORE1_AUDIO_ERROR;

	while (true) {
		__asm volatile ("wfi");
	}
}

static int core1_audio_start(void)
{
	uint32_t *vector_table = (uint32_t *)CPU1_SRAM_ADDR;
	uint32_t sp = CPU1_SRAM_ADDR + CPU1_SRAM_SIZE - 8U;
	uint32_t pc = (uint32_t)core1_audio_worker | 1U;
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	int ret;

	memset((void *)shared, 0, sizeof(*shared));
	memset(vector_table, 0, CORE1_VECTOR_BYTES);
	vector_table[0] = sp;
	vector_table[1] = pc;
	__DSB();
	__ISB();

	ret = core1_reset();
	if (ret != 0) {
		return ret;
	}
	return core1_boot((uint32_t)vector_table, sp, pc);
}

SYS_INIT(core1_audio_start, APPLICATION, 90);

static const char *audio_state_name(uint32_t state)
{
	switch (state) {
	case PICO_CLIP_CORE1_AUDIO_IDLE: return "idle";
	case PICO_CLIP_CORE1_AUDIO_INIT: return "init";
	case PICO_CLIP_CORE1_AUDIO_PLAYING: return "playing";
	case PICO_CLIP_CORE1_AUDIO_DONE: return "done";
	case PICO_CLIP_CORE1_AUDIO_ERROR: return "error";
	default: return "unknown";
	}
}

static int cmd_core1_audio_status(const struct shell *shell, size_t argc, char **argv)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(shell,
		    "core1: magic=0x%08x version=%u boot=%u state=%s err=%d init_stage=%u",
		    shared->magic, shared->version, shared->cpu1_boot_count,
		    audio_state_name(shared->audio_state), shared->audio_error,
		    core1_audio_debug_stage);
	shell_print(shell,
		    "opus: seq=%u len=%u encoded=%u dropped=%u bitrate=%u heartbeat=%u",
		    shared->opus_seq, shared->opus_len, shared->opus_encode_count,
		    shared->opus_dropped, shared->opus_bitrate,
		    shared->audio_heartbeat);
	return 0;
}

SHELL_CMD_REGISTER(core1_audio_status, NULL, "Show Core1 audio status",
		   cmd_core1_audio_status);
