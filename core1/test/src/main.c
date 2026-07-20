#if !defined(CORE1_TEST_BENCHMARK)
#include "core1_audio.h"
#endif
#if defined(CORE1_TEST_BIRTHDAY)
#include "audio_pio.h"
#endif

int audio_noop_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

int core1_opus_benchmark(void);

int main(void)
{
#if defined(CORE1_TEST_BENCHMARK)
	return core1_opus_benchmark();
#elif defined(CORE1_TEST_BIRTHDAY)
	int ret = core1_audio_init();
	if (ret != 0) return ret;
	Happy_Birthday_Out();
#else
	return core1_audio_run_loopback();
#endif
	return 0;
}
