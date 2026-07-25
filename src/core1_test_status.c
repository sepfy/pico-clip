// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <hardware/address_mapped.h>
#include <hardware/regs/io_qspi.h>
#include <hardware/regs/sio.h>
#include <hardware/structs/ioqspi.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>

#include "core1_audio.h"
#include "pico_clip_core1_test_shared.h"

#define CPU1_SRAM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CPU1_SRAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CORE1_FIFO_TIMEOUT_LOOPS 100000U
#define CORE1_VECTOR_BYTES 0x100U
#define BOOTSEL_POLL_MS 10U
#define BOOTSEL_PAUSE_TIMEOUT_US 1000U

static __ramfunc bool bootsel_read(void)
{
	const uint32_t oe_mask = IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS;
	uint32_t primask;
	uint32_t ctrl;
	bool pressed;

	__asm volatile ("mrs %0, primask\ncpsid i" : "=r" (primask) :: "memory");
	ctrl = ioqspi_hw->io[1].ctrl;
	ioqspi_hw->io[1].ctrl =
		(ctrl & ~oe_mask) |
		(IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_DISABLE <<
		 IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);
	for (volatile uint32_t i = 0; i < 1000u; i++) {
	}
	pressed = (sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS) == 0u;
	ioqspi_hw->io[1].ctrl = ctrl;
	__asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
	return pressed;
}

static bool wait_for_pause_ack(
	volatile struct pico_clip_core1_test_shared *shared, uint32_t token)
{
	for (uint32_t i = 0; i < BOOTSEL_PAUSE_TIMEOUT_US; i++) {
		if (shared->flash_pause_ack == token) return true;
		k_busy_wait(1);
	}
	return false;
}

static void bootsel_ptt_thread(void *p1, void *p2, void *p3)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	uint32_t token = 0u;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (true) {
		if (shared->magic != PICO_CLIP_CORE1_TEST_MAGIC ||
		    shared->version != PICO_CLIP_CORE1_TEST_VERSION) {
			k_msleep(BOOTSEL_POLL_MS);
			continue;
		}
		token += 2u;
		shared->flash_pause_request = token;
		__DMB();
		__SEV();
		if (wait_for_pause_ack(shared, token)) {
			shared->mic_enabled = bootsel_read() ? 1u : 0u;
			shared->bootsel_samples++;
		}
		shared->flash_pause_request = token + 1u;
		__DMB();
		__SEV();
		(void)wait_for_pause_ack(shared, token + 1u);
		k_msleep(BOOTSEL_POLL_MS);
	}
}

K_THREAD_DEFINE(bootsel_ptt_tid, 1024, bootsel_ptt_thread,
		NULL, NULL, NULL, 7, 0, 500);

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
	ret = core1_audio_run_pcmu_stream();
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
		    "pcmu: seq=%u len=%u encoded=%u decoded=%u tx_drop=%u rx_drop=%u pending=%u bitrate=%u heartbeat=%u",
		    shared->opus_seq, shared->opus_len, shared->opus_encode_count,
		    shared->opus_decode_count, shared->opus_dropped,
		    shared->spk_opus_dropped,
		    shared->spk_opus_write_seq - shared->spk_opus_read_seq,
		    shared->opus_bitrate,
		    shared->audio_heartbeat);
	shell_print(shell,
		    "pcmu timing us: enc=%u max=%u dec=%u max=%u dec_err=%u queue_max=%u",
		    shared->opus_encode_last_us, shared->opus_encode_max_us,
		    shared->opus_decode_last_us, shared->opus_decode_max_us,
		    shared->opus_decode_errors, shared->spk_opus_pending_max);
	shell_print(shell, "ptt: bootsel=%s samples=%u pause=%u/%u",
		    shared->mic_enabled != 0u ? "pressed" : "released",
		    shared->bootsel_samples, shared->flash_pause_ack,
		    shared->flash_pause_request);
	return 0;
}

SHELL_CMD_REGISTER(core1_audio_status, NULL, "Show Core1 audio status",
		   cmd_core1_audio_status);
