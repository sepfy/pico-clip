#include "core1_audio.h"
#include "core1_opus.h"

#include "DEV_Config.h"
#include "audio_pio.h"
#include "es8311.h"

volatile uint32_t core1_audio_debug_stage;

int core1_audio_init(void)
{
	core1_audio_debug_stage = 1U;
	if (DEV_Module_Init() != 0U) {
		return -1;
	}

	core1_audio_debug_stage = 2U;
	Es8311_Init(pico_audio);
	core1_audio_debug_stage = 3U;
	if (Es8311_Sample_Frequency_Config(pico_audio.mclk_freq,
					    pico_audio.sample_freq) != 0) {
		return -2;
	}
	core1_audio_debug_stage = 4U;
	Es8311_Microphone_Config();
	core1_audio_debug_stage = 5U;
	Es8311_Voice_Volume_Set(pico_audio.volume);
	core1_audio_debug_stage = 6U;
	Es8311_Microphone_Gain_Set(pico_audio.mic_gain);
	core1_audio_debug_stage = 7U;
	return 0;
}

int core1_audio_run_loopback(void)
{
	int ret = core1_audio_init();

	if (ret != 0) {
		return ret;
	}
	Loopback_Test();
	return 0;
}

void core1_audio_run_opus_loopback(void)
{
	core1_opus_loopback();
}
