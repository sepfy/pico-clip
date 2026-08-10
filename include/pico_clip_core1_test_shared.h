/* SPDX-License-Identifier: Apache-2.0 */

#ifndef PICO_CLIP_CORE1_TEST_SHARED_H_
#define PICO_CLIP_CORE1_TEST_SHARED_H_

#include <stdint.h>

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

#define PICO_CLIP_CORE1_TEST_SHM_BASE 0x20080000u
#define PICO_CLIP_CORE1_TEST_MAGIC 0x54314350u /* PC1T */
#define PICO_CLIP_CORE1_TEST_VERSION 6u

#define PICO_CLIP_CORE1_AUDIO_CMD_NONE 0u
#define PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY 1u
#define PICO_CLIP_CORE1_AUDIO_CMD_STOP 2u

#define PICO_CLIP_CORE1_AUDIO_IDLE 0u
#define PICO_CLIP_CORE1_AUDIO_INIT 1u
#define PICO_CLIP_CORE1_AUDIO_PLAYING 2u
#define PICO_CLIP_CORE1_AUDIO_DONE 3u
#define PICO_CLIP_CORE1_AUDIO_ERROR 0xffu

#define PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY BIT(0)
#define PICO_CLIP_CORE1_AUDIO_FLAG_SPK_READY BIT(1)

#define PICO_CLIP_CORE1_OPUS_SAMPLE_RATE 24000u
#define PICO_CLIP_CORE1_OPUS_CHANNELS 1u
#define PICO_CLIP_CORE1_OPUS_FRAME_SAMPLES 480u
#define PICO_CLIP_CORE1_OPUS_BITRATE 16000u
#define PICO_CLIP_CORE1_OPUS_MAX_PACKET 400u
#define PICO_CLIP_CORE1_SPK_OPUS_QUEUE 4u

struct pico_clip_core1_test_shared {
	uint32_t magic;
	uint32_t version;
	uint32_t cpu1_boot_count;
	uint32_t cpu0_audio_cmd_seq;
	uint32_t cpu0_audio_cmd;
	uint32_t cpu1_audio_ack_seq;
	uint32_t audio_state;
	int32_t audio_error;
	uint32_t audio_heartbeat;
	uint32_t audio_play_count;
	uint32_t audio_progress;
	uint32_t audio_flags;
	uint32_t opus_seq;
	uint32_t opus_len;
	uint32_t opus_silence;
	uint32_t opus_bitrate;
	uint32_t opus_encode_count;
	uint32_t opus_decode_count;
	uint32_t opus_dropped;
	uint32_t opus_encode_last_us;
	uint32_t opus_encode_max_us;
	uint32_t opus_decode_last_us;
	uint32_t opus_decode_max_us;
	uint32_t opus_decode_errors;
	uint32_t spk_opus_pending_max;
	uint32_t mic_enabled;
	uint32_t flash_pause_request;
	uint32_t flash_pause_ack;
	uint32_t bootsel_samples;
	uint32_t spk_opus_write_seq;
	uint32_t spk_opus_read_seq;
	uint32_t spk_opus_dropped;
	uint32_t spk_opus_len[PICO_CLIP_CORE1_SPK_OPUS_QUEUE];
	uint32_t spk_opus_slot_seq[PICO_CLIP_CORE1_SPK_OPUS_QUEUE];
	uint8_t opus_packet[PICO_CLIP_CORE1_OPUS_MAX_PACKET];
	uint8_t spk_opus_packet[PICO_CLIP_CORE1_SPK_OPUS_QUEUE][PICO_CLIP_CORE1_OPUS_MAX_PACKET];
};

static inline volatile struct pico_clip_core1_test_shared *pico_clip_core1_test_shm(void)
{
	return (volatile struct pico_clip_core1_test_shared *)PICO_CLIP_CORE1_TEST_SHM_BASE;
}

#endif /* PICO_CLIP_CORE1_TEST_SHARED_H_ */
