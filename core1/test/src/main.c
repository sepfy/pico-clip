#include "core1_audio.h"
#include "audio_pio.h"

int audio_noop_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

int main(void)
{
	int ret = core1_audio_init();

	if (ret != 0) {
		return ret;
	}
#if defined(CORE1_TEST_BIRTHDAY)
	Happy_Birthday_Out();
#else
	core1_audio_run_loopback();
#endif
	return 0;
}
