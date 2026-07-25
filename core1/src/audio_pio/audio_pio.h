/*****************************************************************************
* | File      	:   audio_pio.c
* | Author      :   Waveshare Team
* | Function    :   ES8311 control related PIO interface
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2026-04-01
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#ifndef _PICO_AUDIO_PIO_H
#define _PICO_AUDIO_PIO_H

#if defined(CORE1_TEST_BIRTHDAY)
#include "pico/stdlib.h"
#else
#include <stdbool.h>
#include <stdint.h>
#endif
#include "hardware/pio.h"
#include "hardware/clocks.h"

#define AUDIO_PIO __CONCAT(pio, PICO_AUDIO_PIO)
#define GPIO_FUNC_PIOx __CONCAT(GPIO_FUNC_PIO, PICO_AUDIO_PIO)

#ifndef PICO_MCLK_FREQ
#define PICO_MCLK_FREQ      (8000 * 256)
#endif
#ifndef PICO_SAMPLE_FREQ
#define PICO_SAMPLE_FREQ    8000
#endif
#define PICO_AUDIO_VOLUME   70
#define PICO_AUDIO_COUNT    1
#define PICO_AUDIO_RES_IN   16
#define PICO_AUDIO_RES_OUT  16
#define PICO_AUDIO_MIC_GAIN 3
#define PICO_AUDIO_DOUT     1
#define PICO_AUDIO_DIN      2
#define PICO_AUDIO_MCLK     3
#define PICO_AUDIO_LRCLK    5
#define PICO_AUDIO_BCLK     4
#define PICO_AUDIO_PIO_1    0
#define PICO_AUDIO_PIO_2    0
#define PICO_AUDIO_SM_DOUT  0
#define PICO_AUDIO_SM_DIN   1
#define PICO_AUDIO_SM_MCLK  2

typedef struct pico_audio_struct 
{
    uint32_t mclk_freq;  
    uint32_t sample_freq;    
    uint8_t  res_in;
    uint8_t  res_out;    
    uint8_t  mic_gain;   
    int      volume;  
    uint8_t  channel_count; 
	uint8_t  audio_dout;
	uint8_t  audio_din;
	uint8_t  audio_mclk;
	uint8_t  audio_lrclk;
	uint8_t  audio_bclk;
	PIO	     pio_1;
	PIO	     pio_2;
	uint8_t  sm_dout; 
	uint8_t  sm_din; 
	uint8_t  sm_mclk; 
}pico_audio_t;

static pico_audio_t pico_audio = {
    .mclk_freq = PICO_MCLK_FREQ,        // Master clock frequency
    .sample_freq = PICO_SAMPLE_FREQ,    // Sample frequency
    .channel_count = PICO_AUDIO_COUNT,  // Number of channels
    .res_in = PICO_AUDIO_RES_IN,        // Input bit depth
    .res_out = PICO_AUDIO_RES_OUT,      // Output bit depth
    .mic_gain = PICO_AUDIO_MIC_GAIN,    // Input gain
    .volume = PICO_AUDIO_VOLUME,        // Output volume
    .audio_dout = PICO_AUDIO_DOUT,      // Data output pin
    .audio_din = PICO_AUDIO_DIN,        // Data input pin
    .audio_mclk = PICO_AUDIO_MCLK,      // Master clock pin
    .audio_lrclk = PICO_AUDIO_LRCLK,    // Left and right channel clock pins
    .audio_bclk = PICO_AUDIO_BCLK,      // Bit clock pin
    .pio_1 = pio1,                      // PIO1 instance
    .pio_2 = pio2,                      // PIO2 instance
    .sm_dout = PICO_AUDIO_SM_DOUT,      // Output state machine number
    .sm_din = PICO_AUDIO_SM_DIN,        // Input state machine number
    .sm_mclk = PICO_AUDIO_SM_MCLK       // Clock state machine number
};

/**
 * @brief Initialize PIO output state machine for audio data transmission
 */
void Dout_Pio_Init();

/**
 * @brief Initialize PIO input state machine for audio data reception
 */
void Din_Pio_Init();

/**
 * @brief Initialize PIO Master Clock (MCLK) output state machine
 */
void Mclk_Pio_Init();

/**
 * @brief Set the MCLK frequency by configuring PIO clock divider
 * @param frequency Desired MCLK frequency in Hz
 */
void Set_Mclk_Frequency(uint32_t frequency);

#if !defined(PICO_CLIP_ZEPHYR_CORE1)
/**
 * @brief Play Happy Birthday audio sequence using DMA
 */
void Happy_Birthday_Out();
#endif

/**
 * @brief Audio loopback test - record and playback simultaneously using DMA
 */
void Loopback_Test();

#endif //_PICO_AUDIO_PIO_H
