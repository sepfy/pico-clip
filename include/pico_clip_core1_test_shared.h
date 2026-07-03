/* SPDX-License-Identifier: Apache-2.0 */

#ifndef PICO_CLIP_CORE1_TEST_SHARED_H_
#define PICO_CLIP_CORE1_TEST_SHARED_H_

#include <stdint.h>

#define PICO_CLIP_CORE1_TEST_SHM_BASE 0x20080000u
#define PICO_CLIP_CORE1_TEST_MAGIC 0x54314350u /* PC1T */
#define PICO_CLIP_CORE1_TEST_VERSION 1u

#define PICO_CLIP_CORE1_AUDIO_CMD_NONE 0u
#define PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY 1u
#define PICO_CLIP_CORE1_AUDIO_CMD_STOP 2u

#define PICO_CLIP_CORE1_AUDIO_IDLE 0u
#define PICO_CLIP_CORE1_AUDIO_INIT 1u
#define PICO_CLIP_CORE1_AUDIO_PLAYING 2u
#define PICO_CLIP_CORE1_AUDIO_DONE 3u
#define PICO_CLIP_CORE1_AUDIO_ERROR 0xffu

struct pico_clip_core1_test_shared {
	uint32_t magic;
	uint32_t version;
	uint32_t cpu1_boot_count;
	uint32_t cpu1_heartbeat;
	uint32_t cpu0_cmd_seq;
	uint32_t cpu0_cmd_value;
	uint32_t cpu1_ack_seq;
	uint32_t cpu1_response_value;
	uint32_t cpu0_audio_cmd_seq;
	uint32_t cpu0_audio_cmd;
	uint32_t cpu1_audio_ack_seq;
	uint32_t audio_state;
	int32_t audio_error;
	uint32_t audio_heartbeat;
	uint32_t audio_play_count;
	uint32_t audio_progress;
};

static inline volatile struct pico_clip_core1_test_shared *pico_clip_core1_test_shm(void)
{
	return (volatile struct pico_clip_core1_test_shared *)PICO_CLIP_CORE1_TEST_SHM_BASE;
}

#endif /* PICO_CLIP_CORE1_TEST_SHARED_H_ */
