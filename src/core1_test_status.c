// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <hardware/address_mapped.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>

#include "pico_clip_core1_test_shared.h"

#define CPU1_SRAM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_sram_cpu1_partition))
#define CPU1_SRAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram_cpu1_partition))

#define CORE1_FIFO_TIMEOUT_LOOPS 100000U

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

static void __attribute__((noreturn, noinline)) core1_bare_worker(void)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	shm->magic = PICO_CLIP_CORE1_TEST_MAGIC;
	shm->version = PICO_CLIP_CORE1_TEST_VERSION;
	shm->cpu1_boot_count++;

	while (true) {
		uint32_t cmd_seq;
		uint32_t ack_seq;

		shm->cpu1_heartbeat++;

		cmd_seq = shm->cpu0_cmd_seq;
		ack_seq = shm->cpu1_ack_seq;
		if (cmd_seq != ack_seq) {
			shm->cpu1_ack_seq = cmd_seq;
			shm->cpu1_response_value = shm->cpu0_cmd_value + 1U;
		}

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

static int cmd_core1_test(const struct shell *sh, size_t argc, char **argv)
{
	volatile struct pico_clip_core1_test_shared *shm = pico_clip_core1_test_shm();

	if (argc < 2) {
		shell_print(sh, "Usage: core1_test start_bare|status");
		return -EINVAL;
	}

	if (strcmp(argv[1], "start_bare") == 0) {
		return core1_start_bare(sh);
	}

	if (strcmp(argv[1], "status") != 0) {
		shell_print(sh, "Usage: core1_test start_bare|status");
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
	return 0;
}

SHELL_CMD_REGISTER(core1_test, NULL, "Core1 bare-metal test", cmd_core1_test);
