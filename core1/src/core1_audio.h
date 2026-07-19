#ifndef PICO_CLIP_CORE1_AUDIO_H
#define PICO_CLIP_CORE1_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public boundary used by both the standalone Pico SDK test and the future
 * Zephyr-started Core1 worker.  No application main() or Zephyr API belongs
 * in core1/src.
 */
int core1_audio_init(void);
extern volatile uint32_t core1_audio_debug_stage;

/* Initializes the codec and runs the DMA-IRQ loopback indefinitely. */
int core1_audio_run_loopback(void);
void core1_audio_run_opus_loopback(void);
int core1_audio_run_opus_stream(void);


#ifdef __cplusplus
}
#endif

#endif
