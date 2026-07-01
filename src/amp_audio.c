#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <opus.h>

#include "amp_audio_sample.h"
#include "amp_audio_shared.h"

#define AMP_AUDIO_TEST_FRAME_SAMPLES 320U
#define AMP_AUDIO_TEST_BITRATE 24000
#define AMP_AUDIO_TEST_ENCODER_BYTES 20480
#define AMP_AUDIO_TEST_STACK_SIZE 8192
#define AMP_AUDIO_TEST_PRIORITY 7

static atomic_t sample_play_enabled;
static uint8_t sample_opus_encoder_storage[AMP_AUDIO_TEST_ENCODER_BYTES] __aligned(8);
static OpusEncoder *const sample_opus_encoder =
	(OpusEncoder *)sample_opus_encoder_storage;
static uint8_t sample_opus_packet[AMP_AUDIO_OPUS_MAX_PACKET];
static int16_t sample_pcm_frame[AMP_AUDIO_TEST_FRAME_SAMPLES];
static uint32_t sample_frame_index;
static uint32_t sample_packets_sent;
static uint32_t sample_packets_dropped;
static int sample_last_error;

static const char *amp_audio_state_name(uint32_t state)
{
	switch (state) {
	case AMP_AUDIO_STATE_BOOTING:
		return "booting";
	case AMP_AUDIO_STATE_IDLE:
		return "idle";
	case AMP_AUDIO_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static int amp_audio_queue_speaker_opus(const uint8_t *data, uint32_t len)
{
	volatile struct amp_audio_shared *shared = amp_audio_shared_get();
	uint32_t write_seq;
	uint32_t read_seq;
	uint32_t slot;

	if (shared->magic != AMP_AUDIO_MAGIC ||
	    shared->version != AMP_AUDIO_VERSION ||
	    (shared->flags & AMP_AUDIO_FLAG_SPK_READY) == 0U) {
		return -ENODEV;
	}

	if (len == 0U || len > AMP_AUDIO_OPUS_MAX_PACKET) {
		return -EINVAL;
	}

	write_seq = shared->spk_opus_write_seq;
	read_seq = shared->spk_opus_read_seq;
	if ((write_seq - read_seq) >= AMP_AUDIO_SPK_OPUS_QUEUE) {
		return -EAGAIN;
	}

	slot = write_seq % AMP_AUDIO_SPK_OPUS_QUEUE;
	shared->spk_opus_slot_seq[slot] = 0;
	memcpy((void *)shared->spk_opus_packet[slot], data, len);
	shared->spk_opus_len[slot] = len;
	shared->spk_opus_slot_seq[slot] = write_seq + 1U;
	shared->spk_opus_write_seq = write_seq + 1U;
	shared->spk_opus_seq = write_seq + 1U;
	shared->spk_packets++;
	return 0;
}

static void amp_audio_sample_copy_frame(void)
{
	for (uint32_t i = 0; i < AMP_AUDIO_TEST_FRAME_SAMPLES; i++) {
		uint32_t sample_index = sample_frame_index + i;

		if (sample_index >= AMP_AUDIO_SAMPLE_COUNT) {
			sample_index -= AMP_AUDIO_SAMPLE_COUNT;
		}
		sample_pcm_frame[i] = amp_audio_sample[sample_index];
	}

	sample_frame_index += AMP_AUDIO_TEST_FRAME_SAMPLES;
	if (sample_frame_index >= AMP_AUDIO_SAMPLE_COUNT) {
		sample_frame_index -= AMP_AUDIO_SAMPLE_COUNT;
	}
}

static int amp_audio_sample_encoder_init(void)
{
	int ret;

	ret = opus_encoder_get_size(AMP_AUDIO_SAMPLE_CHANNELS);
	if (ret <= 0 || ret > (int)sizeof(sample_opus_encoder_storage)) {
		return ret <= 0 ? -EINVAL : -ENOMEM;
	}

	ret = opus_encoder_init(sample_opus_encoder, AMP_AUDIO_SAMPLE_RATE,
				AMP_AUDIO_SAMPLE_CHANNELS, OPUS_APPLICATION_AUDIO);
	if (ret != OPUS_OK) {
		return ret;
	}

	opus_encoder_ctl(sample_opus_encoder, OPUS_SET_BITRATE(AMP_AUDIO_TEST_BITRATE));
	opus_encoder_ctl(sample_opus_encoder, OPUS_SET_COMPLEXITY(1));
	opus_encoder_ctl(sample_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
	return 0;
}

static void amp_audio_sample_player(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	sample_last_error = amp_audio_sample_encoder_init();
	while (sample_last_error != 0) {
		k_msleep(1000);
		sample_last_error = amp_audio_sample_encoder_init();
	}

	while (1) {
		int ret;

		if (!atomic_get(&sample_play_enabled)) {
			k_msleep(20);
			continue;
		}

		amp_audio_sample_copy_frame();
		ret = opus_encode(sample_opus_encoder, sample_pcm_frame,
				  AMP_AUDIO_TEST_FRAME_SAMPLES, sample_opus_packet,
				  sizeof(sample_opus_packet));
		if (ret < 0) {
			sample_last_error = ret;
			sample_packets_dropped++;
			k_msleep(20);
			continue;
		}

		for (int tries = 0; tries < 20; tries++) {
			sample_last_error =
				amp_audio_queue_speaker_opus(sample_opus_packet, (uint32_t)ret);
			if (sample_last_error == 0) {
				sample_packets_sent++;
				break;
			}
			k_msleep(1);
		}
		if (sample_last_error != 0) {
			sample_packets_dropped++;
		}

		k_msleep(20);
	}
}

K_THREAD_DEFINE(amp_audio_sample_tid, AMP_AUDIO_TEST_STACK_SIZE,
		amp_audio_sample_player, NULL, NULL, NULL,
		AMP_AUDIO_TEST_PRIORITY, 0, 0);

static int cmd_amp_audio(const struct shell *sh, size_t argc, char **argv)
{
	volatile struct amp_audio_shared *shared = amp_audio_shared_get();
	uint32_t heartbeat0;
	uint32_t heartbeat1;

	if (argc < 2) {
		shell_print(sh, "Usage: amp_audio status | tone <on|off> | playwav <on|off>");
		return -EINVAL;
	}

	if (shared->magic != AMP_AUDIO_MAGIC || shared->version != AMP_AUDIO_VERSION) {
		shell_error(sh, "audio core not ready: magic=0x%08x version=%u",
			    shared->magic, shared->version);
		return -ENODEV;
	}

	if (strcmp(argv[1], "tone") == 0) {
		ARG_UNUSED(argc);
		ARG_UNUSED(argv);
		shared->flags &= ~AMP_AUDIO_FLAG_TEST_TONE;
		atomic_clear(&sample_play_enabled);
		shell_error(sh, "speaker output disabled in this firmware");
		return -ENOTSUP;
	}

	if (strcmp(argv[1], "playwav") == 0) {
		ARG_UNUSED(argc);
		ARG_UNUSED(argv);
		shared->flags &= ~AMP_AUDIO_FLAG_TEST_TONE;
		atomic_clear(&sample_play_enabled);
		shell_error(sh, "speaker output disabled in this firmware");
		return -ENOTSUP;
	}

	if (argc != 2 || strcmp(argv[1], "status") != 0) {
		shell_print(sh, "Usage: amp_audio status | tone <on|off> | playwav <on|off>");
		return -EINVAL;
	}

	heartbeat0 = shared->heartbeat;
	k_busy_wait(2000);
	heartbeat1 = shared->heartbeat;

	shell_print(sh, "audio core: state=%s heartbeat=%u->%u %s",
		    amp_audio_state_name(shared->state), heartbeat0, heartbeat1,
		    heartbeat0 == heartbeat1 ? "stalled" : "running");
	shell_print(sh, "layout: cpu1_sram=0x%08x+0x%x shared=0x%08x+0x%x",
		    shared->cpu1_sram_base, shared->cpu1_sram_size,
		    shared->shared_base, shared->shared_size);
	shell_print(sh, "audio: rate=%u channels=%u frame=%u flags=0x%x err=%d",
		    shared->sample_rate, shared->channels, shared->frame_samples,
		    shared->flags, shared->last_error);
		shell_print(sh, "stats: mic_frames=%u mic_packets=%u spk_packets=%u spk_frames=%u overrun=%u underrun=%u",
			    shared->mic_frames, shared->mic_packets, shared->spk_packets,
			    shared->spk_frames, shared->overrun_count, shared->underrun_count);
		shell_print(sh, "mic: min=%d max=%d abs_avg=%u nonzero=%u",
			    shared->mic_min, shared->mic_max,
			    shared->mic_abs_avg, shared->mic_nonzero);
		shell_print(sh, "opus: seq=%u len=%u checksum=0x%08x encode_us=%u max_us=%u bitrate=%u dropped=%u enc_size=%u",
			    shared->opus_seq, shared->opus_len, shared->opus_checksum,
			    shared->opus_encode_us, shared->opus_max_encode_us,
		    shared->opus_bitrate, shared->opus_dropped,
		    shared->opus_encoder_size);
			shell_print(sh, "speaker: seq=%u pending=%u len=%u decode_us=%u max_us=%u dropped=%u dec_size=%u",
				    shared->spk_opus_seq,
				    shared->spk_opus_write_seq - shared->spk_opus_read_seq,
			    shared->spk_opus_len[shared->spk_opus_read_seq %
						 AMP_AUDIO_SPK_OPUS_QUEUE],
				    shared->spk_decode_us, shared->spk_max_decode_us,
				    shared->spk_opus_dropped, shared->spk_decoder_size);
			shell_print(sh, "playwav: enabled=%ld sent=%u dropped=%u err=%d frame=%u",
				    atomic_get(&sample_play_enabled), sample_packets_sent,
				    sample_packets_dropped, sample_last_error,
				    sample_frame_index);

	return 0;
}

SHELL_CMD_REGISTER(amp_audio, NULL, "AMP audio core status", cmd_amp_audio);
