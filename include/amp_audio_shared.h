#ifndef WIFI_SHELL_AMP_AUDIO_SHARED_H_
#define WIFI_SHELL_AMP_AUDIO_SHARED_H_

#include <stdint.h>

#define AMP_AUDIO_SHARED_BASE 0x20078000u
#define AMP_AUDIO_SHARED_SIZE 0x8000u
#define AMP_AUDIO_CPU0_SRAM_BASE 0x20000000u
#define AMP_AUDIO_CPU0_SRAM_SIZE 0x60000u
#define AMP_AUDIO_CPU1_SRAM_BASE 0x20060000u
#define AMP_AUDIO_CPU1_SRAM_SIZE 0x18000u
#define AMP_AUDIO_RESERVED_BASE 0x20080000u
#define AMP_AUDIO_RESERVED_SIZE 0x2000u
#define AMP_AUDIO_CPU1_FLASH_BASE 0x10200000u
#define AMP_AUDIO_CPU1_FLASH_SIZE 0x40000u
#define AMP_AUDIO_OPUS_MAX_PACKET 400u
#define AMP_AUDIO_SPK_OPUS_QUEUE 4u

#define AMP_AUDIO_MAGIC 0x31445541u /* AUD1 */
#define AMP_AUDIO_VERSION 1u

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

#define AMP_AUDIO_FLAG_OPUS_READY BIT(0)
#define AMP_AUDIO_FLAG_TEST_TONE BIT(1)
#define AMP_AUDIO_FLAG_PIO_MIC BIT(2)
#define AMP_AUDIO_FLAG_SPK_READY BIT(3)

enum amp_audio_state {
	AMP_AUDIO_STATE_BOOTING = 1,
	AMP_AUDIO_STATE_IDLE = 2,
	AMP_AUDIO_STATE_ERROR = 0xff,
};

struct amp_audio_shared {
	uint32_t magic;
	uint32_t version;
	uint32_t state;
	uint32_t heartbeat;
	uint32_t cpu1_sram_base;
	uint32_t cpu1_sram_size;
	uint32_t shared_base;
	uint32_t shared_size;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t frame_samples;
	uint32_t flags;
	uint32_t mic_frames;
	uint32_t mic_packets;
	uint32_t spk_packets;
	uint32_t spk_frames;
	uint32_t overrun_count;
	uint32_t underrun_count;
	int32_t last_error;
	uint32_t opus_seq;
	uint32_t opus_len;
	uint32_t opus_checksum;
	uint32_t opus_encode_us;
	uint32_t opus_max_encode_us;
	uint32_t opus_bitrate;
	uint32_t opus_dropped;
	uint32_t opus_encoder_size;
	uint8_t opus_packet[AMP_AUDIO_OPUS_MAX_PACKET];
	uint32_t mic_abs_avg;
	int32_t mic_min;
	int32_t mic_max;
	uint32_t mic_nonzero;
	uint32_t spk_opus_seq;
	uint32_t spk_opus_write_seq;
	uint32_t spk_opus_read_seq;
	uint32_t spk_opus_dropped;
	uint32_t spk_decode_us;
	uint32_t spk_max_decode_us;
	uint32_t spk_decoder_size;
	uint32_t spk_opus_len[AMP_AUDIO_SPK_OPUS_QUEUE];
	uint32_t spk_opus_slot_seq[AMP_AUDIO_SPK_OPUS_QUEUE];
	uint8_t spk_opus_packet[AMP_AUDIO_SPK_OPUS_QUEUE][AMP_AUDIO_OPUS_MAX_PACKET];
};

static inline volatile struct amp_audio_shared *amp_audio_shared_get(void)
{
	return (volatile struct amp_audio_shared *)AMP_AUDIO_SHARED_BASE;
}

#endif
