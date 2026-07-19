#include "core1_opus.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <opus.h>

#include "audio_pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "pico_clip_core1_test_shared.h"

#define CORE1_OPUS_SAMPLE_RATE   24000
#define CORE1_OPUS_CHANNELS      1
#define CORE1_OPUS_FRAME_SAMPLES 480
#define CORE1_OPUS_BITRATE       24000
#define CORE1_OPUS_PACKET_BYTES  256
#define CORE1_OPUS_DELAY_MS      500
#define CORE1_OPUS_DELAY_FRAMES  (CORE1_OPUS_DELAY_MS / 20)
#define CORE1_OPUS_ENCODER_BYTES 15616
#define CORE1_WORK_BASE          0x20070100u
#define CORE1_STREAM_ENCODER     ((OpusEncoder *)CORE1_WORK_BASE)
#define CORE1_STREAM_PCM         \
	((int16_t *)(CORE1_WORK_BASE + CORE1_OPUS_ENCODER_BYTES))
#define CORE1_STREAM_PACKET      \
	((uint8_t *)(CORE1_WORK_BASE + CORE1_OPUS_ENCODER_BYTES + \
		      2U * CORE1_OPUS_FRAME_SAMPLES * sizeof(int16_t)))
#define CORE1_STREAM_WORK_END    \
	(CORE1_WORK_BASE + CORE1_OPUS_ENCODER_BYTES + \
	 2U * CORE1_OPUS_FRAME_SAMPLES * sizeof(int16_t) + \
	 CORE1_OPUS_PACKET_BYTES)
#define CORE1_STACK_LIMIT        0x20080000u
#define CORE1_STACK_GUARD_BYTES  8192u

static int16_t capture_pcm[2][CORE1_OPUS_FRAME_SAMPLES];
static int16_t playback_pcm[2][CORE1_OPUS_FRAME_SAMPLES];
static uint8_t opus_packets[CORE1_OPUS_DELAY_FRAMES][CORE1_OPUS_PACKET_BYTES];
static int16_t opus_packet_lengths[CORE1_OPUS_DELAY_FRAMES];

static int rx_dma_chan;
static int tx_dma_chan;
static dma_channel_config rx_dma_config;
static dma_channel_config tx_dma_config;
static volatile uint32_t capture_ready_mask;
static volatile uint32_t capture_write_index;
static volatile uint32_t playback_free_mask;
static volatile uint32_t playback_pending_mask;
static volatile int32_t playback_active_index;

static int stream_rx_dma_chan;
static dma_channel_config stream_rx_dma_config;
static volatile uint32_t stream_ready_mask;
static volatile uint32_t stream_write_index;
static volatile uint32_t stream_dropped;

static void start_capture(uint32_t index)
{
	dma_channel_configure(rx_dma_chan, &rx_dma_config,
			      capture_pcm[index],
			      &pico_audio.pio_1->rxf[pico_audio.sm_din],
			      CORE1_OPUS_FRAME_SAMPLES, true);
}

static void start_playback(uint32_t index)
{
	playback_active_index = (int32_t)index;
	dma_channel_configure(tx_dma_chan, &tx_dma_config,
			      &pico_audio.pio_2->txf[pico_audio.sm_dout],
			      playback_pcm[index], CORE1_OPUS_FRAME_SAMPLES, true);
}

static void opus_dma_irq_handler(void)
{
	uint32_t pending = dma_hw->ints0;
	uint32_t rx_mask = 1u << rx_dma_chan;
	uint32_t tx_mask = 1u << tx_dma_chan;

	if ((pending & rx_mask) != 0U) {
		uint32_t completed = capture_write_index;

		dma_hw->ints0 = rx_mask;
		capture_ready_mask |= 1u << completed;
		capture_write_index ^= 1U;
		start_capture(capture_write_index);
	}

	if ((pending & tx_mask) != 0U) {
		uint32_t completed = (uint32_t)playback_active_index;

		dma_hw->ints0 = tx_mask;
		playback_free_mask |= 1u << completed;
		if (playback_pending_mask != 0U) {
			uint32_t next = (playback_pending_mask & 1U) ? 0U : 1U;

			playback_pending_mask &= ~(1u << next);
			start_playback(next);
		} else {
			playback_active_index = -1;
		}
	}

	/* Preserve an event if it happens just before the foreground executes WFE. */
	__asm volatile ("sev");
}

static int32_t take_playback_buffer(void)
{
	uint32_t free_buffers;

	irq_set_enabled(DMA_IRQ_0, false);
	free_buffers = playback_free_mask;
	if (free_buffers != 0U) {
		uint32_t index = (free_buffers & 1U) ? 0U : 1U;

		playback_free_mask &= ~(1u << index);
		irq_set_enabled(DMA_IRQ_0, true);
		return (int32_t)index;
	}
	irq_set_enabled(DMA_IRQ_0, true);
	return -1;
}

static void queue_playback_buffer(uint32_t index)
{
	irq_set_enabled(DMA_IRQ_0, false);
	if (playback_active_index < 0) {
		start_playback(index);
	} else {
		playback_pending_mask |= 1u << index;
	}
	irq_set_enabled(DMA_IRQ_0, true);
}

static uint32_t take_capture_buffer(void)
{
	uint32_t ready;

	irq_set_enabled(DMA_IRQ_0, false);
	ready = capture_ready_mask;
	if (ready != 0U) {
		uint32_t index = (ready & 1U) ? 0U : 1U;

		capture_ready_mask &= ~(1u << index);
		irq_set_enabled(DMA_IRQ_0, true);
		return index + 1U;
	}
	irq_set_enabled(DMA_IRQ_0, true);
	return 0U;
}

void core1_opus_loopback(void)
{
	OpusEncoder *encoder;
	OpusDecoder *decoder;
	int opus_error;
	uint32_t packet_write_index = 0U;
	uint32_t packet_read_index = 0U;
	uint32_t buffered_packets = 0U;

	encoder = opus_encoder_create(CORE1_OPUS_SAMPLE_RATE, CORE1_OPUS_CHANNELS,
				      OPUS_APPLICATION_AUDIO, &opus_error);
	if (encoder == NULL || opus_error != OPUS_OK) {
		return;
	}
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(CORE1_OPUS_BITRATE));
	opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
	opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	decoder = opus_decoder_create(CORE1_OPUS_SAMPLE_RATE, CORE1_OPUS_CHANNELS,
				      &opus_error);
	if (decoder == NULL || opus_error != OPUS_OK) {
		opus_encoder_destroy(encoder);
		return;
	}

	Mclk_Pio_Init();
	Din_Pio_Init();
	Dout_Pio_Init();
	pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_din, true);
	pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout, true);

	rx_dma_chan = dma_claim_unused_channel(true);
	rx_dma_config = dma_channel_get_default_config(rx_dma_chan);
	channel_config_set_transfer_data_size(&rx_dma_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_dma_config, false);
	channel_config_set_write_increment(&rx_dma_config, true);
	channel_config_set_dreq(&rx_dma_config,
		pio_get_dreq(pico_audio.pio_1, pico_audio.sm_din, false));

	tx_dma_chan = dma_claim_unused_channel(true);
	tx_dma_config = dma_channel_get_default_config(tx_dma_chan);
	channel_config_set_transfer_data_size(&tx_dma_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_dma_config, true);
	channel_config_set_write_increment(&tx_dma_config, false);
	channel_config_set_dreq(&tx_dma_config,
		pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true));

	dma_channel_set_irq0_enabled(rx_dma_chan, true);
	dma_channel_set_irq0_enabled(tx_dma_chan, true);
	dma_hw->ints0 = (1u << rx_dma_chan) | (1u << tx_dma_chan);
	irq_set_exclusive_handler(DMA_IRQ_0, opus_dma_irq_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	capture_ready_mask = 0U;
	capture_write_index = 0U;
	playback_free_mask = 3U;
	playback_pending_mask = 0U;
	playback_active_index = -1;
	pio_sm_clear_fifos(pico_audio.pio_1, pico_audio.sm_din);
	start_capture(capture_write_index);

	while (true) {
		uint32_t token = take_capture_buffer();
		int packet_len;
		int decoded_samples;
		int32_t playback_index;

		if (token == 0U) {
			__asm volatile ("wfe");
			continue;
		}

		packet_len = opus_encode(encoder, capture_pcm[token - 1U],
					 CORE1_OPUS_FRAME_SAMPLES,
					 opus_packets[packet_write_index],
					 CORE1_OPUS_PACKET_BYTES);
		if (packet_len < 0) {
			continue;
		}
		opus_packet_lengths[packet_write_index] = (int16_t)packet_len;
		packet_write_index = (packet_write_index + 1U) % CORE1_OPUS_DELAY_FRAMES;
		if (buffered_packets < CORE1_OPUS_DELAY_FRAMES) {
			buffered_packets++;
		}
		if (buffered_packets < CORE1_OPUS_DELAY_FRAMES) {
			continue;
		}

		do {
			playback_index = take_playback_buffer();
			if (playback_index < 0) {
				__asm volatile ("wfe");
			}
		} while (playback_index < 0);

		packet_len = opus_packet_lengths[packet_read_index];
		decoded_samples = opus_decode(decoder,
					      opus_packets[packet_read_index], packet_len,
					      playback_pcm[playback_index],
					      CORE1_OPUS_FRAME_SAMPLES, 0);
		packet_read_index = (packet_read_index + 1U) % CORE1_OPUS_DELAY_FRAMES;
		if (decoded_samples != CORE1_OPUS_FRAME_SAMPLES) {
			irq_set_enabled(DMA_IRQ_0, false);
			playback_free_mask |= 1u << playback_index;
			irq_set_enabled(DMA_IRQ_0, true);
			continue;
		}

		queue_playback_buffer((uint32_t)playback_index);
	}
}

static void stream_start_capture(uint32_t index)
{
	dma_channel_configure(stream_rx_dma_chan, &stream_rx_dma_config,
			      &CORE1_STREAM_PCM[index * CORE1_OPUS_FRAME_SAMPLES],
			      &pico_audio.pio_1->rxf[pico_audio.sm_din],
			      CORE1_OPUS_FRAME_SAMPLES, true);
}

static void stream_dma_irq_handler(void)
{
	uint32_t mask = 1u << stream_rx_dma_chan;
	uint32_t completed;
	uint32_t next;

	if ((dma_hw->ints0 & mask) == 0U) {
		return;
	}
	dma_hw->ints0 = mask;
	completed = stream_write_index;
	next = completed ^ 1U;
	if ((stream_ready_mask & (1u << next)) != 0U) {
		/* The encoder did not consume the next buffer within 20 ms. */
		stream_ready_mask &= ~(1u << next);
		stream_dropped++;
	}
	stream_ready_mask |= 1u << completed;
	stream_write_index = next;
	stream_start_capture(next);
	__asm volatile ("sev");
}

static uint32_t stream_take_capture(void)
{
	uint32_t ready;
	uint32_t index;

	nvic_hw->icer[DMA_IRQ_0 / 32U] = 1u << (DMA_IRQ_0 % 32U);
	ready = stream_ready_mask;
	if (ready == 0U) {
		nvic_hw->iser[DMA_IRQ_0 / 32U] = 1u << (DMA_IRQ_0 % 32U);
		return 2U;
	}
	index = (ready & 1U) ? 0U : 1U;
	stream_ready_mask &= ~(1u << index);
	nvic_hw->iser[DMA_IRQ_0 / 32U] = 1u << (DMA_IRQ_0 % 32U);
	return index;
}

static uint32_t stream_checksum(const uint8_t *data, uint32_t len)
{
	uint32_t checksum = 2166136261u;

	for (uint32_t i = 0; i < len; i++) {
		checksum = (checksum ^ data[i]) * 16777619u;
	}
	return checksum;
}

int core1_opus_stream(void)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	OpusEncoder *encoder = CORE1_STREAM_ENCODER;
	uint32_t sequence = 0U;
	int encoder_size;
	int ret;

	if (CORE1_STREAM_WORK_END > CORE1_STACK_LIMIT - CORE1_STACK_GUARD_BYTES) {
		return OPUS_ALLOC_FAIL;
	}
	encoder_size = opus_encoder_get_size(CORE1_OPUS_CHANNELS);
	shared->opus_encoder_size = encoder_size > 0 ? (uint32_t)encoder_size : 0U;
	if (encoder_size <= 0 || encoder_size > CORE1_OPUS_ENCODER_BYTES) {
		return OPUS_ALLOC_FAIL;
	}
	ret = opus_encoder_init(encoder, CORE1_OPUS_SAMPLE_RATE,
				CORE1_OPUS_CHANNELS, OPUS_APPLICATION_VOIP);
	if (ret != OPUS_OK) {
		return ret;
	}
	(void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(CORE1_OPUS_BITRATE));
	(void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
	(void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	Mclk_Pio_Init();
	Din_Pio_Init();
	pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_din, true);
	pio_sm_clear_fifos(pico_audio.pio_1, pico_audio.sm_din);

	stream_rx_dma_chan = dma_claim_unused_channel(true);
	stream_rx_dma_config = dma_channel_get_default_config(stream_rx_dma_chan);
	channel_config_set_transfer_data_size(&stream_rx_dma_config, DMA_SIZE_16);
	channel_config_set_read_increment(&stream_rx_dma_config, false);
	channel_config_set_write_increment(&stream_rx_dma_config, true);
	channel_config_set_dreq(&stream_rx_dma_config,
		pio_get_dreq(pico_audio.pio_1, pico_audio.sm_din, false));
	dma_channel_set_irq0_enabled(stream_rx_dma_chan, true);
	dma_hw->ints0 = 1u << stream_rx_dma_chan;
	((uintptr_t *)scb_hw->vtor)[16U + DMA_IRQ_0] =
		(uintptr_t)stream_dma_irq_handler;
	__asm volatile ("dmb" ::: "memory");
	nvic_hw->iser[DMA_IRQ_0 / 32U] = 1u << (DMA_IRQ_0 % 32U);

	stream_ready_mask = 0U;
	stream_write_index = 0U;
	stream_dropped = 0U;
	shared->opus_seq = 0U;
	shared->opus_len = 0U;
	shared->opus_bitrate = CORE1_OPUS_BITRATE;
	shared->opus_encode_count = 0U;
	shared->opus_dropped = 0U;
	shared->audio_flags |= PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY;
	shared->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	stream_start_capture(0U);

	while (true) {
		uint32_t index = stream_take_capture();
		int packet_len;

		if (index > 1U) {
			__asm volatile ("wfe");
			continue;
		}
		packet_len = opus_encode(encoder,
					 &CORE1_STREAM_PCM[index * CORE1_OPUS_FRAME_SAMPLES],
					 CORE1_OPUS_FRAME_SAMPLES, CORE1_STREAM_PACKET,
					 CORE1_OPUS_PACKET_BYTES);
		if (packet_len < 0) {
			shared->audio_error = packet_len;
			shared->opus_dropped++;
			continue;
		}

		/* Zero marks the single shared slot invalid while Core1 updates it. */
		shared->opus_seq = 0U;
		__asm volatile ("dmb" ::: "memory");
		memcpy((void *)shared->opus_packet, CORE1_STREAM_PACKET,
		       (size_t)packet_len);
		shared->opus_len = (uint32_t)packet_len;
		shared->opus_checksum = stream_checksum(CORE1_STREAM_PACKET,
						   (uint32_t)packet_len);
		shared->opus_encode_count++;
		shared->opus_dropped = stream_dropped;
		shared->audio_heartbeat++;
		__asm volatile ("dmb" ::: "memory");
		shared->opus_seq = ++sequence;
	}
}
