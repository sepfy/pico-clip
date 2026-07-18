#ifndef _ES8311_H_
#define _ES8311_H_

#include "audio_pio.h"

/*
 *   ES8311_REGISTER NAME_REG_REGISTER ADDRESS
 */
#define ES8311_RESET_REG00              0x00  /*reset digital,csm,clock manager etc.*/

/*
 * Clock Scheme Register definition
 */
#define ES8311_CLK_MANAGER_REG01        0x01 /* select clk src for mclk, enable clock for codec */
#define ES8311_CLK_MANAGER_REG02        0x02 /* clk divider and clk multiplier */
#define ES8311_CLK_MANAGER_REG03        0x03 /* adc fsmode and osr  */
#define ES8311_CLK_MANAGER_REG04        0x04 /* dac osr */
#define ES8311_CLK_MANAGER_REG05        0x05 /* clk divier for adc and dac */
#define ES8311_CLK_MANAGER_REG06        0x06 /* bclk inverter and divider */
#define ES8311_CLK_MANAGER_REG07        0x07 /* tri-state, lrck divider */
#define ES8311_CLK_MANAGER_REG08        0x08 /* lrck divider */
/*
 * SDP
 */
#define ES8311_SDPIN_REG09              0x09 /* dac serial digital port */
#define ES8311_SDPOUT_REG0A             0x0A /* adc serial digital port */
/*
 * SYSTEM
 */
#define ES8311_SYSTEM_REG0B             0x0B /* system */
#define ES8311_SYSTEM_REG0C             0x0C /* system */
#define ES8311_SYSTEM_REG0D             0x0D /* system, power up/down */
#define ES8311_SYSTEM_REG0E             0x0E /* system, power up/down */
#define ES8311_SYSTEM_REG0F             0x0F /* system, low power */
#define ES8311_SYSTEM_REG10             0x10 /* system */
#define ES8311_SYSTEM_REG11             0x11 /* system */
#define ES8311_SYSTEM_REG12             0x12 /* system, Enable DAC */
#define ES8311_SYSTEM_REG13             0x13 /* system */
#define ES8311_SYSTEM_REG14             0x14 /* system, select DMIC, select analog pga gain */
/*
 * ADC
 */
#define ES8311_ADC_REG15                0x15 /* ADC, adc ramp rate, dmic sense */
#define ES8311_ADC_REG16                0x16 /* ADC */
#define ES8311_ADC_REG17                0x17 /* ADC, volume */
#define ES8311_ADC_REG18                0x18 /* ADC, alc enable and winsize */
#define ES8311_ADC_REG19                0x19 /* ADC, alc maxlevel */
#define ES8311_ADC_REG1A                0x1A /* ADC, alc automute */
#define ES8311_ADC_REG1B                0x1B /* ADC, alc automute, adc hpf s1 */
#define ES8311_ADC_REG1C                0x1C /* ADC, equalizer, hpf s2 */
/*
 * DAC
 */
#define ES8311_DAC_REG31                0x31 /* DAC, mute */
#define ES8311_DAC_REG32                0x32 /* DAC, volume */
#define ES8311_DAC_REG33                0x33 /* DAC, offset */
#define ES8311_DAC_REG34                0x34 /* DAC, drc enable, drc winsize */
#define ES8311_DAC_REG35                0x35 /* DAC, drc maxlevel, minilevel */
#define ES8311_DAC_REG37                0x37 /* DAC, ramprate */
/*
 *GPIO
 */
#define ES8311_GPIO_REG44               0x44 /* GPIO, dac2adc for test */
#define ES8311_GP_REG45                 0x45 /* GP CONTROL */
/*
 * CHIP
 */
#define ES8311_CHD1_REGFD               0xFD /* CHIP ID1 */
#define ES8311_CHD2_REGFE               0xFE /* CHIP ID2 */
#define ES8311_CHVER_REGFF              0xFF /* VERSION */

#define ES8311_MAX_REGISTER             0xFF
#define ES8311_I2C_ADDR                 0x18

/*************************************************************/

typedef enum {
    ES8311_MIC_GAIN_MIN = -1,
    ES8311_MIC_GAIN_0DB,
    ES8311_MIC_GAIN_6DB,
    ES8311_MIC_GAIN_12DB,
    ES8311_MIC_GAIN_18DB,
    ES8311_MIC_GAIN_24DB,
    ES8311_MIC_GAIN_30DB,
    ES8311_MIC_GAIN_36DB,
    ES8311_MIC_GAIN_42DB,
    ES8311_MIC_GAIN_MAX
} es8311_mic_gain_t;

typedef enum {
    ES8311_FADE_OFF = 0,
    ES8311_FADE_4LRCK, // 4LRCK means ramp 0.25dB/4LRCK
    ES8311_FADE_8LRCK,
    ES8311_FADE_16LRCK,
    ES8311_FADE_32LRCK,
    ES8311_FADE_64LRCK,
    ES8311_FADE_128LRCK,
    ES8311_FADE_256LRCK,
    ES8311_FADE_512LRCK,
    ES8311_FADE_1024LRCK,
    ES8311_FADE_2048LRCK,
    ES8311_FADE_4096LRCK,
    ES8311_FADE_8192LRCK,
    ES8311_FADE_16384LRCK,
    ES8311_FADE_32768LRCK,
    ES8311_FADE_65536LRCK
} es8311_fade_t;

typedef enum es8311_resolution_t {
    ES8311_RESOLUTION_16 = 16,
    ES8311_RESOLUTION_18 = 18,
    ES8311_RESOLUTION_20 = 20,
    ES8311_RESOLUTION_24 = 24,
    ES8311_RESOLUTION_32 = 32
} es8311_resolution_t;

/**
 * @brief Initialize ES8311
 *
 * Initializes ES8311 audio codec with clock configuration, audio format setup,
 * and powers up analog circuitry and DAC output.
 *
 * @param pico_audio Audio configuration structure containing sample rate, resolution, and MCLK frequency
 */
void Es8311_Init(pico_audio_t pico_audio);

/**
 * @brief Set output volume
 *
 * Sets DAC output volume with automatic clamping to valid range.
 * Volume parameter outside <0, 100> interval will be truncated.
 *
 * @param volume Output volume level (0 ~ 100)
 * @return int Volume value that was set. Same as input, unless input was outside <0, 100> interval
 */
int Es8311_Voice_Volume_Set(int volume);

/**
 * @brief Get current output volume
 *
 * Reads the current DAC output volume setting.
 *
 * @return int Current volume level (0 ~ 100)
 */
int Es8311_Voice_Volume_Get();

/**
 * @brief Print ES8311 register contents for debugging
 *
 * Reads and prints all ES8311 register values from 0x00 to 0x4A.
 */
void Es8311_Register_Dump();

/**
 * @brief Mute or unmute ES8311 DAC output
 *
 * Controls the mute state of the DAC output.
 *
 * @param mute true to mute output, false to unmute
 */
void Es8311_Voice_Mute(bool mute);

/**
 * @brief Set microphone gain
 *
 * Sets the ADC input gain for microphone.
 *
 * @param gain_db Microphone gain level to set
 */
void Es8311_Microphone_Gain_Set(es8311_mic_gain_t gain_db);

/**
 * @brief Configure microphone input
 *
 * Enables analog microphone input and sets maximum PGA gain.
 * Optionally enables PDM digital microphone mode.
 */
void Es8311_Microphone_Config();

/**
 * @brief Configure sampling frequency
 *
 * Configures clock dividers to support the specified sample frequency.
 * This function is called by Es8311_Init(). Call explicitly only if you want to change
 * sample frequency during runtime.
 *
 * @param mclk_frequency MCLK frequency in Hz (MCLK or SCLK pin, depending on register configuration)
 * @param sample_frequency Required sample frequency in Hz (e.g., 44100, 22050, 48000, etc.)
 * @return int 0 on success, -1 if the specified frequency combination is not supported
 */
int Es8311_Sample_Frequency_Config(int mclk_frequency, int sample_frequency);

/**
 * @brief Configure fade in/out for DAC output
 *
 * Sets the ramp rate for audio fade in/out on the DAC output.
 *
 * @param fade Fade ramp rate configuration
 */
void Es8311_Voice_Fade(const es8311_fade_t fade);

/**
 * @brief Configure fade in/out for ADC input
 *
 * Sets the ramp rate for audio fade in/out on the ADC input (microphone).
 *
 * @param fade Fade ramp rate configuration
 */
void Es8311_Microphone_Fade(const es8311_fade_t fade);

/**
 * @brief Read ES8311 chip ID
 * @return uint16_t 16-bit chip ID value
 */
uint16_t Es8311_Read_Id();

#endif // !_ES8311_H_
