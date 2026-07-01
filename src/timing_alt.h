#ifndef WIFI_SHELL_TIMING_ALT_H
#define WIFI_SHELL_TIMING_ALT_H

#include <stdint.h>

struct mbedtls_timing_hr_time {
	int64_t ms;
};

typedef struct mbedtls_timing_delay_context {
	struct mbedtls_timing_hr_time timer;
	uint32_t int_ms;
	uint32_t fin_ms;
} mbedtls_timing_delay_context;

#endif /* WIFI_SHELL_TIMING_ALT_H */
