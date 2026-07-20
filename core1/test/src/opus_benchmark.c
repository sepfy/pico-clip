#include <stdint.h>

#include <opus.h>

#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "pico/time.h"

#define BENCH_UART uart0
#define BENCH_UART_TX_PIN 16
#define BENCH_UART_RX_PIN 17
#define BENCH_RATE 24000
#define BENCH_CHANNELS 1
#define BENCH_20MS_SAMPLES 480
#define BENCH_40MS_SAMPLES 960
#define BENCH_PACKET_BYTES 400
#define BENCH_ITERATIONS 200
#define BENCH_ENCODER_BYTES 32000
#define BENCH_DECODER_BYTES 19000
#define BENCH_REMOTE_SAMPLES 960

static uint8_t encoder_storage[BENCH_ENCODER_BYTES] __attribute__((aligned(8)));
static uint8_t decoder_storage[BENCH_DECODER_BYTES] __attribute__((aligned(8)));
static int16_t pcm[BENCH_40MS_SAMPLES];
static int16_t decoded[BENCH_40MS_SAMPLES];
static int16_t remote_stereo[BENCH_REMOTE_SAMPLES * 2];
static uint8_t packet[BENCH_PACKET_BYTES];

static void uart_text(const char *text)
{
	uart_puts(BENCH_UART, text);
}

static void uart_u32(uint32_t value)
{
	char digits[10];
	uint32_t count = 0;

	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);
	while (count != 0U) {
		uart_putc_raw(BENCH_UART, digits[--count]);
	}
}

static void report(const char *name, uint64_t total_us, uint32_t max_us,
		   uint32_t count)
{
	uart_text(name);
	uart_text(": avg_us=");
	uart_u32((uint32_t)(total_us / count));
	uart_text(" max_us=");
	uart_u32(max_us);
	uart_text("\r\n");
}

static int encode_once(OpusEncoder *encoder, int samples, uint32_t *elapsed_us)
{
	uint64_t start = time_us_64();
	int len = opus_encode(encoder, pcm, samples, packet, sizeof(packet));

	*elapsed_us = (uint32_t)(time_us_64() - start);
	return len;
}

static int decode_once(OpusDecoder *decoder, int len, int samples,
		       uint32_t *elapsed_us)
{
	uint64_t start = time_us_64();
	int ret = opus_decode(decoder, packet, len, decoded, samples, 0);

	*elapsed_us = (uint32_t)(time_us_64() - start);
	return ret;
}

static int benchmark_remote_decode(OpusDecoder *decoder, int rate,
				   int frame_samples, int packet_len,
				   const char *name)
{
	uint64_t total_us = 0U;
	uint32_t max_us = 0U;
	int ret = opus_decoder_init(decoder, rate, 1);

	if (ret != OPUS_OK) return ret;
	for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
		uint32_t elapsed_us;

		ret = decode_once(decoder, packet_len, frame_samples, &elapsed_us);
		if (ret != frame_samples) return ret < 0 ? ret : OPUS_INTERNAL_ERROR;
		total_us += elapsed_us;
		if (elapsed_us > max_us) max_us = elapsed_us;
	}
	report(name, total_us, max_us, BENCH_ITERATIONS);
	return 0;
}

int core1_opus_benchmark(void)
{
	OpusEncoder *encoder = (OpusEncoder *)encoder_storage;
	OpusDecoder *decoder = (OpusDecoder *)decoder_storage;
	uint32_t seed = 1U;
	uint64_t encode20_total = 0U;
	uint64_t decode20_total = 0U;
	uint64_t encode40_total = 0U;
	uint32_t encode20_max = 0U;
	uint32_t decode20_max = 0U;
	uint32_t encode40_max = 0U;
	int remote_packet_len;
	int ret;

	if (!set_sys_clock_khz(200000, true)) {
		return -5;
	}
	uart_init(BENCH_UART, 115200);
	gpio_set_function(BENCH_UART_TX_PIN, GPIO_FUNC_UART);
	gpio_set_function(BENCH_UART_RX_PIN, GPIO_FUNC_UART);
	for (uint32_t i = 0; i < BENCH_40MS_SAMPLES; i++) {
		seed = seed * 1664525U + 1013904223U;
		pcm[i] = (int16_t)(seed >> 18);
	}
	for (uint32_t i = 0; i < BENCH_REMOTE_SAMPLES; i++) {
		seed = seed * 1664525U + 1013904223U;
		remote_stereo[2U * i] = (int16_t)(seed >> 17);
		seed = seed * 1664525U + 1013904223U;
		remote_stereo[2U * i + 1U] = (int16_t)(seed >> 17);
	}

	ret = opus_encoder_get_size(BENCH_CHANNELS);
	if (ret <= 0 || ret > BENCH_ENCODER_BYTES) {
		uart_text("encoder storage error\r\n");
		return -1;
	}
	ret = opus_decoder_get_size(BENCH_CHANNELS);
	if (ret <= 0 || ret > BENCH_DECODER_BYTES) {
		uart_text("decoder storage error\r\n");
		return -2;
	}
	ret = opus_encoder_init(encoder, BENCH_RATE, BENCH_CHANNELS,
				OPUS_APPLICATION_VOIP);
	if (ret != OPUS_OK) return ret;
	ret = opus_decoder_init(decoder, BENCH_RATE, BENCH_CHANNELS);
	if (ret != OPUS_OK) return ret;
	(void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
	(void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
	(void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	uart_text("clk_sys_hz=");
	uart_u32(clock_get_hz(clk_sys));
	uart_text("\r\nOpus benchmark: 24kHz mono, complexity=0, iterations=200\r\n");
	for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
		uint32_t encode_us;
		uint32_t decode_us;
		int len = encode_once(encoder, BENCH_20MS_SAMPLES, &encode_us);

		if (len < 0 || decode_once(decoder, len, BENCH_20MS_SAMPLES,
					 &decode_us) != BENCH_20MS_SAMPLES) {
			uart_text("20ms codec error\r\n");
			return -3;
		}
		encode20_total += encode_us;
		decode20_total += decode_us;
		if (encode_us > encode20_max) encode20_max = encode_us;
		if (decode_us > decode20_max) decode20_max = decode_us;
	}
	for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
		uint32_t encode_us;
		int len = encode_once(encoder, BENCH_40MS_SAMPLES, &encode_us);

		if (len < 0) {
			uart_text("40ms encode error\r\n");
			return -4;
		}
		encode40_total += encode_us;
		if (encode_us > encode40_max) encode40_max = encode_us;
	}
	report("encode_20ms", encode20_total, encode20_max, BENCH_ITERATIONS);
	report("decode_20ms", decode20_total, decode20_max, BENCH_ITERATIONS);
	report("encode_40ms", encode40_total, encode40_max, BENCH_ITERATIONS);

	ret = opus_encoder_init(encoder, 48000, 2, OPUS_APPLICATION_AUDIO);
	if (ret != OPUS_OK) return ret;
	(void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
	(void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
	remote_packet_len = opus_encode(encoder, remote_stereo,
					BENCH_REMOTE_SAMPLES, packet,
					sizeof(packet));
	if (remote_packet_len < 0) return remote_packet_len;
	uart_text("remote_48k_stereo_packet_bytes=");
	uart_u32((uint32_t)remote_packet_len);
	uart_text("\r\n");
	ret = benchmark_remote_decode(decoder, 24000, 480, remote_packet_len,
				      "remote_decode_24k_mono");
	if (ret != 0) return ret;
	ret = benchmark_remote_decode(decoder, 16000, 320, remote_packet_len,
				      "remote_decode_16k_mono");
	if (ret != 0) return ret;
	ret = benchmark_remote_decode(decoder, 8000, 160, remote_packet_len,
				      "remote_decode_8k_mono");
	if (ret != 0) return ret;
	uart_text("done\r\n");
	return 0;
}
