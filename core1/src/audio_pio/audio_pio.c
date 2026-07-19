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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(PICO_CLIP_ZEPHYR_CORE1)
#include "pico/stdlib.h"
#endif
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "audio_pio.h"
#include "audio_data.h"
#include "audio_pio.pio.h"
#include "music.h"
#include "DEV_Config.h"

/**
 * @brief Set the MCLK frequency by configuring PIO clock divider
 * @param mclk_freq Desired MCLK frequency in Hz
 */
void Set_Mclk_Frequency(uint32_t mclk_freq)
{
	double system_clock_frequency = clock_get_hz(clk_sys);
    double div = (system_clock_frequency / mclk_freq) / 5; 
    pio_sm_set_clkdiv(pico_audio.pio_1, pico_audio.sm_mclk, div);
}

/**
 * @brief Process 16-bit audio data into 32-bit format for PIO output
 * @param audio Pointer to 16-bit audio data array
 * @param len Number of samples in the audio array
 * @return int32_t* Pointer to newly allocated 32-bit audio data array
 */
int32_t* Data_Treating(const int16_t *audio , uint32_t len)
{
	int32_t *samples = (int32_t *)calloc(len, sizeof(int32_t));
	for(uint32_t i = 0; i < len; i++)
	{
		if(pico_audio.channel_count == 1)
		{
			samples[i] = audio[i] * 65536;
		}
		else
		{
			samples[i] = audio[i] * 65536 + audio[i];
		}
	}
	return samples;
}

/**
 * @brief Output audio samples via PIO state machine
 * @param samples Pointer to 32-bit audio sample array
 * @param len Number of samples to output
 */
void Audio_Out(int32_t *samples, int32_t len) 
{
	for(uint16_t i = 0; i < len; i++)
	   	pio_sm_put_blocking(pico_audio.pio_2, pico_audio.sm_dout, samples[i]);
}

/**
 * @brief Initialize PIO output state machine for audio data transmission
 */
void Dout_Pio_Init()
{
    pio_sm_claim(pico_audio.pio_2, pico_audio.sm_dout);
    uint offset = pio_add_program(pico_audio.pio_2, &audio_pio_program);
	audio_pio_program_init(pico_audio.pio_2, pico_audio.sm_dout , offset, pico_audio.audio_dout, pico_audio.audio_lrclk);
	pio_sm_set_clkdiv(pico_audio.pio_2, pico_audio.sm_dout, 1.0f);
    pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout , true);
}

/**
 * @brief Initialize PIO input state machine for audio data reception
 */
void Din_Pio_Init()
{
    pio_sm_claim(pico_audio.pio_1, pico_audio.sm_din);
    uint offset = pio_add_program(pico_audio.pio_1, &read_pio_program);
	read_pio_program_init(pico_audio.pio_1, pico_audio.sm_din , offset, pico_audio.audio_din, pico_audio.audio_lrclk);
    pio_sm_set_clkdiv(pico_audio.pio_1, pico_audio.sm_din, 1.0f);
    pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_din , true);
}

/**
 * @brief Initialize PIO Master Clock (MCLK) output state machine
 */
void Mclk_Pio_Init()
{
    pio_sm_claim(pico_audio.pio_1, pico_audio.sm_mclk);
    uint offset = pio_add_program(pico_audio.pio_1, &mclk_pio_program);
    mclk_pio_program_init(pico_audio.pio_1, pico_audio.sm_mclk, offset, pico_audio.audio_mclk);
    Set_Mclk_Frequency(pico_audio.mclk_freq);
    pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_mclk , true);
}

/**
 * @brief Play Happy Birthday audio sequence using DMA
 */
void Happy_Birthday_Out()
{
    // MCLK
    Mclk_Pio_Init();
    // WRITE
    Dout_Pio_Init(); 
	int len = 124800;

    channel_config_set_dreq(&dma_config, pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true));
    
    while (true) 
    {	
        // Configure DMA transfers
        dma_channel_configure(
            dma_channel,
            &dma_config,
            &pico_audio.pio_2->txf[pico_audio.sm_dout], // Destination address: PIO TX FIFO
            Happy_birsday,                       // Source address: audio data array
            len,                                 // Number of samples to transfer
            true                                 // Start immediately
        );

        dma_channel_wait_for_finish_blocking(dma_channel);
    }
}

/**
 * @brief Output a 440Hz sine wave test signal
 */
void Sine_440hz_Out()
{
    // MCLK
    Mclk_Pio_Init();
    // WRITE
    Dout_Pio_Init(); 
	int len = 24000;

    channel_config_set_dreq(&dma_config, pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true));

    while (true) 
    {	
        // Configure DMA transfers
        dma_channel_configure(
            dma_channel,
            &dma_config,
            &pico_audio.pio_2->txf[pico_audio.sm_dout], // Destination address: PIO TX FIFO
            Sine_440hz,                       // Source address: audio data array
            len,                              // Number of samples to transfer
            true                              // Start immediately
        );

        dma_channel_wait_for_finish_blocking(dma_channel);
    }
}

/**
 * @brief Audio loopback test - record and playback simultaneously using DMA
 */
#define MIC_NUM_SAMPLES 12000

static int16_t mic_sample_buffers[MIC_NUM_SAMPLES];
static int loopback_dma_rx_chan;
static int loopback_dma_tx_chan;
static dma_channel_config loopback_dma_rx_config;
static dma_channel_config loopback_dma_tx_config;

static void Loopback_Dma_Handler(void)
{
    uint32_t pending = dma_hw->ints0;
    uint32_t rx_mask = 1u << loopback_dma_rx_chan;
    uint32_t tx_mask = 1u << loopback_dma_tx_chan;

    if (pending & rx_mask)
    {
        dma_hw->ints0 = rx_mask;
        dma_channel_configure(
            loopback_dma_tx_chan,
            &loopback_dma_tx_config,
            &pico_audio.pio_2->txf[pico_audio.sm_dout],
            mic_sample_buffers,
            MIC_NUM_SAMPLES,
            true
        );
    }

    if (pending & tx_mask)
    {
        dma_hw->ints0 = tx_mask;
        dma_channel_configure(
            loopback_dma_rx_chan,
            &loopback_dma_rx_config,
            mic_sample_buffers,
            &pico_audio.pio_1->rxf[pico_audio.sm_din],
            MIC_NUM_SAMPLES,
            true
        );
    }
}

void Loopback_Test()
{
    // MCLK
    Mclk_Pio_Init();
    // READ
    Din_Pio_Init();
    // WRITE
    Dout_Pio_Init();

    pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_din, true);
    pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout, true);

    // ===== DMA RX（PIO -> RAM）=====
    loopback_dma_rx_chan = dma_claim_unused_channel(true);
    loopback_dma_rx_config = dma_channel_get_default_config(loopback_dma_rx_chan);
    channel_config_set_transfer_data_size(&loopback_dma_rx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&loopback_dma_rx_config, false);
    channel_config_set_write_increment(&loopback_dma_rx_config, true);
    channel_config_set_dreq(
        &loopback_dma_rx_config,
        pio_get_dreq(pico_audio.pio_1, pico_audio.sm_din, false)
    );

    // ===== DMA TX（RAM -> PIO）=====
    loopback_dma_tx_chan = dma_claim_unused_channel(true);
    loopback_dma_tx_config = dma_channel_get_default_config(loopback_dma_tx_chan);
    channel_config_set_transfer_data_size(&loopback_dma_tx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&loopback_dma_tx_config, true);
    channel_config_set_write_increment(&loopback_dma_tx_config, false);
    channel_config_set_dreq(
        &loopback_dma_tx_config,
        pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true)
    );

    dma_channel_set_irq0_enabled(loopback_dma_rx_chan, true);
    dma_channel_set_irq0_enabled(loopback_dma_tx_chan, true);
    dma_hw->ints0 = (1u << loopback_dma_rx_chan) | (1u << loopback_dma_tx_chan);
    ((uintptr_t *)scb_hw->vtor)[16U + DMA_IRQ_0] =
        (uintptr_t)Loopback_Dma_Handler;
    __asm volatile ("dmb" ::: "memory");
    nvic_hw->iser[DMA_IRQ_0 / 32U] = 1u << (DMA_IRQ_0 % 32U);

    // Start with capture. The IRQ handler alternates RX -> TX -> RX.
    dma_channel_configure(
        loopback_dma_rx_chan,
        &loopback_dma_rx_config,
        mic_sample_buffers,
        &pico_audio.pio_1->rxf[pico_audio.sm_din],
        MIC_NUM_SAMPLES,
        true
    );

    while (true) {
        __asm volatile ("wfi");
    }
}

/**
 * @brief Play music audio sequence using DMA
 */
void Music_Out()
{
    // MCLK
    Mclk_Pio_Init();
    // WRITE
    Dout_Pio_Init(); 

    channel_config_set_dreq(&dma_config, pio_get_dreq(pico_audio.pio_2, pico_audio.sm_dout, true));

    while (true) 
    {	
        // Configure DMA transfers
        dma_channel_configure(
            dma_channel,
            &dma_config,
            &pico_audio.pio_2->txf[pico_audio.sm_dout], // Destination address: PIO TX FIFO
            audio_data,                       // Source address: audio data array
            AUDIO_SAMPLES,                    // Number of samples to transfer
            true                              // Start immediately
        );

        dma_channel_wait_for_finish_blocking(dma_channel);
    }
}

/**
 * @brief Stop audio PIO state machines
 */
void __no_inline_not_in_flash_func(Stop)()
{
    pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_mclk , false);
    pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout , false);
}

/**
 * @brief Start audio PIO state machines
 */
void __no_inline_not_in_flash_func(Start)()
{
    pio_sm_set_enabled(pico_audio.pio_1, pico_audio.sm_mclk , true);
    pio_sm_set_enabled(pico_audio.pio_2, pico_audio.sm_dout , true);
}
