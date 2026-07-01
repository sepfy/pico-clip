#include <zephyr/kernel.h>

#include <mbedtls/timing.h>

unsigned long long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time *val, int reset)
{
	int64_t now = k_uptime_get();

	if (reset) {
		val->ms = now;
		return 0;
	}

	return (unsigned long long)(now - val->ms);
}

void mbedtls_timing_set_delay(void *data, uint32_t int_ms, uint32_t fin_ms)
{
	mbedtls_timing_delay_context *ctx = data;

	ctx->int_ms = int_ms;
	ctx->fin_ms = fin_ms;

	if (fin_ms != 0) {
		(void)mbedtls_timing_get_timer(&ctx->timer, 1);
	}
}

int mbedtls_timing_get_delay(void *data)
{
	mbedtls_timing_delay_context *ctx = data;
	unsigned long long elapsed_ms;

	if (ctx->fin_ms == 0) {
		return -1;
	}

	elapsed_ms = mbedtls_timing_get_timer(&ctx->timer, 0);

	if (elapsed_ms >= ctx->fin_ms) {
		return 2;
	}

	if (elapsed_ms >= ctx->int_ms) {
		return 1;
	}

	return 0;
}

uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context *data)
{
	return data->fin_ms;
}
