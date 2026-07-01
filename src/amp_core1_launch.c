// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <hardware/regs/psm.h>
#include <hardware/regs/sio.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>

#include "amp_audio_shared.h"

#define CPU1_VECTOR_TABLE AMP_AUDIO_CPU1_FLASH_BASE
#define CPU1_BOOT_TIMEOUT 1000000U

static bool fifo_pop(uint32_t *value)
{
	uint32_t timeout = CPU1_BOOT_TIMEOUT;

	while ((sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) == 0U) {
		if (timeout-- == 0U) {
			return false;
		}
	}
	*value = sio_hw->fifo_rd;
	return true;
}

static bool fifo_push(uint32_t value)
{
	uint32_t timeout = CPU1_BOOT_TIMEOUT;

	while ((sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS) == 0U) {
		if (timeout-- == 0U) {
			return false;
		}
	}
	sio_hw->fifo_wr = value;
	__asm__ volatile("sev");
	return true;
}

static void fifo_drain(void)
{
	while ((sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) != 0U) {
		(void)sio_hw->fifo_rd;
	}
	sio_hw->fifo_st = SIO_FIFO_ST_ROE_BITS | SIO_FIFO_ST_WOF_BITS;
}

static int core1_reset_to_bootrom(void)
{
	uint32_t value;

	psm_hw->frce_off |= PSM_FRCE_OFF_PROC1_BITS;
	while ((psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS) == 0U) {
	}

	fifo_drain();
	psm_hw->frce_off &= ~PSM_FRCE_OFF_PROC1_BITS;

	if (!fifo_pop(&value) || value != 0U) {
		return -1;
	}
	return 0;
}

static int core1_launch_raw(uint32_t vector_table)
{
	const uint32_t sp = *(const uint32_t *)vector_table;
	const uint32_t entry = *(const uint32_t *)(vector_table + 4U);
	const uint32_t sequence[] = {0U, 0U, 1U, vector_table, sp, entry};
	uint32_t seq = 0;

	while (seq < ARRAY_SIZE(sequence)) {
		uint32_t response;
		uint32_t cmd = sequence[seq];

		if (cmd == 0U) {
			fifo_drain();
			__asm__ volatile("sev");
		}
		if (!fifo_push(cmd) || !fifo_pop(&response)) {
			return -1;
		}
		seq = (response == cmd) ? seq + 1U : 0U;
	}
	return 0;
}

static int cmd_core1_audio_start(const struct shell *sh, size_t argc, char **argv)
{
	volatile struct amp_audio_shared *shared = amp_audio_shared_get();
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shared->magic = 0;
	ret = core1_reset_to_bootrom();
	if (ret != 0) {
		shell_error(sh, "core1 reset failed");
		return ret;
	}
	ret = core1_launch_raw(CPU1_VECTOR_TABLE);
	if (ret != 0) {
		shell_error(sh, "core1 launch failed");
		return ret;
	}

	shell_print(sh, "core1 audio launched at 0x%08x", CPU1_VECTOR_TABLE);
	return 0;
}

static int cmd_core1_audio_status(const struct shell *sh, size_t argc, char **argv)
{
	volatile struct amp_audio_shared *shared = amp_audio_shared_get();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "magic=0x%08x state=0x%x heartbeat=%u err=%d frames=%u rate=%u",
		    shared->magic, shared->state, shared->heartbeat, shared->last_error,
		    shared->spk_frames, shared->sample_rate);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(core1_audio_cmds,
	SHELL_CMD(start, NULL, "Launch CPU1 bare-metal ES8311 birthday player", cmd_core1_audio_start),
	SHELL_CMD(status, NULL, "Show CPU1 audio shared status", cmd_core1_audio_status),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(core1_audio, &core1_audio_cmds, "CPU1 bare-metal audio", NULL);
