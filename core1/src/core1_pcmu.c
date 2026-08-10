#include "core1_pcmu.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_pio.h"
#include "audio_data.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/timer.h"
#include "pico_clip_core1_test_shared.h"

#if defined(PICO_CLIP_ZEPHYR_CORE1)
#include <zephyr/toolchain.h>
#endif

#define PCMU_FRAME_SAMPLES 160u
#define PCMU_SAMPLE_RATE 8000u
#define PCMU_BITRATE 64000u
#define PCMU_WORK_BASE 0x20070100u
#define PCMU_CAPTURE ((int16_t *)PCMU_WORK_BASE)
#define PCMU_PLAYBACK (PCMU_CAPTURE + 2u * PCMU_FRAME_SAMPLES)
#define PCMU_PACKET ((uint8_t *)(PCMU_PLAYBACK + 2u * PCMU_FRAME_SAMPLES))

static int rx_chan;
static int tx_chan;
static dma_channel_config rx_config;
static dma_channel_config tx_config;
static volatile uint32_t capture_ready;
static volatile uint32_t capture_write;
static volatile uint32_t playback_free;
static volatile uint32_t playback_pending;
static volatile int32_t playback_active;
static volatile uint32_t capture_dropped;
static uint32_t audio_cmd_seq;
static bool birthday_playing;
static uint32_t birthday_sample;

#if defined(PICO_CLIP_ZEPHYR_CORE1)
static __ramfunc void wait_for_flash_resume(
	volatile struct pico_clip_core1_test_shared *shared, uint32_t request)
{
	shared->flash_pause_ack = request;
	__asm volatile ("dmb\nsev" ::: "memory");
	while (shared->flash_pause_request == request) {
		__asm volatile ("wfe");
	}
	shared->flash_pause_ack = shared->flash_pause_request;
	__asm volatile ("dmb\nsev" ::: "memory");
}

static void pause_for_flash_if_requested(
	volatile struct pico_clip_core1_test_shared *shared)
{
	uint32_t request = shared->flash_pause_request;

	if (request == shared->flash_pause_ack) return;
	nvic_hw->icer[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	wait_for_flash_resume(shared, request);
	nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
}
#else
static void pause_for_flash_if_requested(
	volatile struct pico_clip_core1_test_shared *shared)
{
	(void)shared;
}
#endif

static uint8_t linear_to_ulaw(int16_t sample)
{
	int32_t pcm = sample;
	uint8_t sign = 0u;
	uint8_t exponent = 7u;
	uint8_t mantissa;
	int32_t mask = 0x4000;

	if (pcm < 0) {
		pcm = -pcm;
		sign = 0x80u;
	}
	if (pcm > 32635) pcm = 32635;
	pcm += 0x84;
	while (exponent != 0u && (pcm & mask) == 0) {
		exponent--;
		mask >>= 1;
	}
	mantissa = (uint8_t)((pcm >> (exponent + 3u)) & 0x0f);
	return (uint8_t)~(sign | (uint8_t)(exponent << 4) | mantissa);
}

static int16_t ulaw_to_linear(uint8_t value)
{
	uint8_t u = (uint8_t)~value;
	int32_t pcm = ((int32_t)(u & 0x0f) << 3) + 0x84;

	pcm <<= (u & 0x70) >> 4;
	return (int16_t)((u & 0x80) ? (0x84 - pcm) : (pcm - 0x84));
}

static void start_capture(uint32_t index)
{
	dma_channel_configure(rx_chan, &rx_config,
			      &PCMU_CAPTURE[index * PCMU_FRAME_SAMPLES],
			      &pico_audio.pio_1->rxf[pico_audio.sm_din],
			      PCMU_FRAME_SAMPLES, true);
}

static void start_playback(uint32_t index)
{
	playback_active = (int32_t)index;
	dma_channel_configure(tx_chan, &tx_config,
			      &pico_audio.pio_2->txf[pico_audio.sm_dout],
			      &PCMU_PLAYBACK[index * PCMU_FRAME_SAMPLES],
			      PCMU_FRAME_SAMPLES, true);
}

static void pcmu_dma_irq(void)
{
	uint32_t pending = dma_hw->ints0;
	uint32_t rx_mask = 1u << rx_chan;
	uint32_t tx_mask = 1u << tx_chan;

	if ((pending & rx_mask) != 0u) {
		uint32_t completed = capture_write;
		uint32_t next = completed ^ 1u;

		dma_hw->ints0 = rx_mask;
		if ((capture_ready & (1u << next)) != 0u) {
			capture_ready &= ~(1u << next);
			capture_dropped++;
		}
		capture_ready |= 1u << completed;
		capture_write = next;
		start_capture(next);
	}
	if ((pending & tx_mask) != 0u) {
		uint32_t completed = (uint32_t)playback_active;

		dma_hw->ints0 = tx_mask;
		playback_free |= 1u << completed;
		if (playback_pending != 0u) {
			uint32_t next = (playback_pending & 1u) ? 0u : 1u;

			playback_pending &= ~(1u << next);
			start_playback(next);
		} else {
			playback_active = -1;
		}
	}
	__asm volatile ("sev");
}

static uint32_t take_capture(void)
{
	uint32_t ready;
	uint32_t index;

	nvic_hw->icer[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	ready = capture_ready;
	if (ready == 0u) {
		nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
		return 2u;
	}
	index = (ready & 1u) ? 0u : 1u;
	capture_ready &= ~(1u << index);
	nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	return index;
}

static int32_t take_playback(void)
{
	uint32_t index;

	nvic_hw->icer[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	if (playback_free == 0u) {
		nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
		return -1;
	}
	index = (playback_free & 1u) ? 0u : 1u;
	playback_free &= ~(1u << index);
	nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	return (int32_t)index;
}

static void queue_playback(uint32_t index)
{
	nvic_hw->icer[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
	if (playback_active < 0) start_playback(index);
	else playback_pending |= 1u << index;
	nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);
}

static void handle_audio_command(
	volatile struct pico_clip_core1_test_shared *shared)
{
	uint32_t seq = shared->cpu0_audio_cmd_seq;

	if (seq == audio_cmd_seq) return;
	audio_cmd_seq = seq;
	switch (shared->cpu0_audio_cmd) {
	case PICO_CLIP_CORE1_AUDIO_CMD_BIRTHDAY:
		birthday_sample = 0u;
		birthday_playing = true;
		shared->audio_progress = 0u;
		shared->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
		break;
	case PICO_CLIP_CORE1_AUDIO_CMD_STOP:
		birthday_playing = false;
		shared->audio_state = PICO_CLIP_CORE1_AUDIO_IDLE;
		break;
	default:
		break;
	}
	shared->cpu1_audio_ack_seq = seq;
}

static bool play_birthday_frame(
	volatile struct pico_clip_core1_test_shared *shared)
{
	int32_t buffer;
	uint32_t i;

	if (!birthday_playing) return false;
	buffer = take_playback();
	if (buffer < 0) return false;
	for (i = 0u; i < PCMU_FRAME_SAMPLES; i++) {
		if (birthday_sample >= HAPPY_BIRTHDAY_SAMPLE_COUNT) break;
		PCMU_PLAYBACK[(uint32_t)buffer * PCMU_FRAME_SAMPLES + i] =
			Happy_birsday[birthday_sample];
		birthday_sample += HAPPY_BIRTHDAY_SAMPLE_RATE / PCMU_SAMPLE_RATE;
	}
	for (; i < PCMU_FRAME_SAMPLES; i++) {
		PCMU_PLAYBACK[(uint32_t)buffer * PCMU_FRAME_SAMPLES + i] = 0;
	}
	shared->audio_progress = birthday_sample;
	shared->audio_play_count++;
	queue_playback((uint32_t)buffer);
	if (birthday_sample >= HAPPY_BIRTHDAY_SAMPLE_COUNT) {
		birthday_playing = false;
		shared->audio_state = PICO_CLIP_CORE1_AUDIO_DONE;
	}
	return true;
}

static bool decode_remote(volatile struct pico_clip_core1_test_shared *shared)
{
	uint32_t read_seq = shared->spk_opus_read_seq;
	uint32_t slot;
	uint32_t len;
	int32_t buffer;
	uint32_t start_us;

	if (read_seq == shared->spk_opus_write_seq) return false;
	buffer = take_playback();
	if (buffer < 0) return false;
	slot = read_seq % PICO_CLIP_CORE1_SPK_OPUS_QUEUE;
	if (shared->spk_opus_slot_seq[slot] != read_seq + 1u) {
		playback_free |= 1u << buffer;
		return false;
	}
	len = shared->spk_opus_len[slot];
	if (len != PCMU_FRAME_SAMPLES) {
		shared->spk_opus_read_seq = read_seq + 1u;
		shared->spk_opus_dropped++;
		shared->opus_decode_errors++;
		playback_free |= 1u << buffer;
		return true;
	}
	memcpy(PCMU_PACKET, (const void *)shared->spk_opus_packet[slot], len);
	__asm volatile ("dmb" ::: "memory");
	shared->spk_opus_read_seq = read_seq + 1u;
	start_us = timer0_hw->timerawl;
	for (uint32_t i = 0; i < PCMU_FRAME_SAMPLES; i++) {
		PCMU_PLAYBACK[(uint32_t)buffer * PCMU_FRAME_SAMPLES + i] =
			ulaw_to_linear(PCMU_PACKET[i]);
	}
	shared->opus_decode_last_us = timer0_hw->timerawl - start_us;
	if (shared->opus_decode_last_us > shared->opus_decode_max_us)
		shared->opus_decode_max_us = shared->opus_decode_last_us;
	shared->opus_decode_count++;
	shared->audio_play_count++;
	queue_playback((uint32_t)buffer);
	return true;
}

int core1_pcmu_stream(void)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	uint32_t sequence = 0u;

	Mclk_Pio_Init();
	Din_Pio_Init();
	Dout_Pio_Init();
	pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_din, true);
	pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout, true);
	pio_sm_clear_fifos(pico_audio.pio_1, pico_audio.sm_din);
	pio_sm_clear_fifos(pico_audio.pio_2, pico_audio.sm_dout);

	rx_chan = dma_claim_unused_channel(true);
	rx_config = dma_channel_get_default_config(rx_chan);
	channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&rx_config, false);
	channel_config_set_write_increment(&rx_config, true);
	channel_config_set_dreq(&rx_config,
		pio_get_dreq(pico_audio.pio_1, pico_audio.sm_din, false));
	tx_chan = dma_claim_unused_channel(true);
	tx_config = dma_channel_get_default_config(tx_chan);
	channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
	channel_config_set_read_increment(&tx_config, true);
	channel_config_set_write_increment(&tx_config, false);
	channel_config_set_dreq(&tx_config,
		pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true));
	dma_channel_set_irq0_enabled(rx_chan, true);
	dma_channel_set_irq0_enabled(tx_chan, true);
	dma_hw->ints0 = (1u << rx_chan) | (1u << tx_chan);
	((uintptr_t *)scb_hw->vtor)[16u + DMA_IRQ_0] = (uintptr_t)pcmu_dma_irq;
	__asm volatile ("dmb" ::: "memory");
	nvic_hw->iser[DMA_IRQ_0 / 32u] = 1u << (DMA_IRQ_0 % 32u);

	capture_ready = 0u;
	capture_write = 0u;
	capture_dropped = 0u;
	playback_free = 3u;
	playback_pending = 0u;
	playback_active = -1;
	shared->opus_seq = 0u;
	shared->opus_len = 0u;
	shared->opus_silence = 0u;
	shared->opus_bitrate = PCMU_BITRATE;
	shared->opus_encode_count = 0u;
	shared->opus_decode_count = 0u;
	shared->opus_dropped = 0u;
	shared->opus_encode_max_us = 0u;
	shared->opus_decode_max_us = 0u;
	shared->opus_decode_errors = 0u;
	shared->spk_opus_pending_max = 0u;
	shared->mic_enabled = 0u;
	shared->flash_pause_ack = shared->flash_pause_request;
	shared->spk_opus_read_seq = shared->spk_opus_write_seq;
	audio_cmd_seq = shared->cpu0_audio_cmd_seq;
	birthday_playing = false;
	birthday_sample = 0u;
	shared->audio_flags |= PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY |
			       PICO_CLIP_CORE1_AUDIO_FLAG_SPK_READY;
	shared->audio_state = PICO_CLIP_CORE1_AUDIO_PLAYING;
	start_capture(0u);

	while (true) {
		pause_for_flash_if_requested(shared);
		handle_audio_command(shared);
		bool did_work = play_birthday_frame(shared);
		if (!birthday_playing) did_work = decode_remote(shared) || did_work;
		uint32_t index = take_capture();
		uint32_t start_us;
		bool mic_enabled;

		if (index > 1u) {
			if (!did_work) __asm volatile ("wfe");
			continue;
		}
		shared->audio_heartbeat++;
		shared->opus_dropped = capture_dropped;
		mic_enabled = shared->mic_enabled != 0u;
		if (mic_enabled) {
			start_us = timer0_hw->timerawl;
			for (uint32_t i = 0; i < PCMU_FRAME_SAMPLES; i++) {
				PCMU_PACKET[i] = linear_to_ulaw(
					PCMU_CAPTURE[index * PCMU_FRAME_SAMPLES + i]);
			}
			shared->opus_encode_last_us = timer0_hw->timerawl - start_us;
			if (shared->opus_encode_last_us > shared->opus_encode_max_us)
				shared->opus_encode_max_us = shared->opus_encode_last_us;
		} else {
			continue;
		}
		shared->opus_seq = 0u;
		__asm volatile ("dmb" ::: "memory");
		memcpy((void *)shared->opus_packet, PCMU_PACKET, PCMU_FRAME_SAMPLES);
		shared->opus_len = PCMU_FRAME_SAMPLES;
		shared->opus_silence = 0u;
		shared->opus_encode_count++;
		__asm volatile ("dmb" ::: "memory");
		shared->opus_seq = ++sequence;
	}
}
