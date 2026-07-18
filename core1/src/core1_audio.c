#include "core1_audio.h"

#include "DEV_Config.h"
#include "audio_pio.h"
#include "es8311.h"

int core1_audio_init(void)
{
	if (DEV_Module_Init() != 0U) {
		return -1;
	}

	Es8311_Init(pico_audio);
	if (Es8311_Sample_Frequency_Config(pico_audio.mclk_freq,
					    pico_audio.sample_freq) != 0) {
		return -2;
	}
	Es8311_Microphone_Config();
	Es8311_Voice_Volume_Set(pico_audio.volume);
	Es8311_Microphone_Gain_Set(pico_audio.mic_gain);
	return 0;
}

void core1_audio_run_loopback(void)
{
	Loopback_Test();
}
