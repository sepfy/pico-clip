#include "core1_audio.h"
#include "audio_pio.h"

int audio_noop_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

int main(void)
{
#if defined(CORE1_TEST_BIRTHDAY)
	int ret = core1_audio_init();
	if (ret != 0) return ret;
	Happy_Birthday_Out();
#elif defined(CORE1_TEST_OPUS)
	int ret = core1_audio_init();
	if (ret != 0) return ret;
	core1_audio_run_opus_loopback();
#else
	return core1_audio_run_loopback();
#endif
	return 0;
}
