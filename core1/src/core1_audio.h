#ifndef PICO_CLIP_CORE1_AUDIO_H
#define PICO_CLIP_CORE1_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public boundary used by both the standalone Pico SDK test and the future
 * Zephyr-started Core1 worker.  No application main() or Zephyr API belongs
 * in core1/src.
 */
int core1_audio_init(void);

/* Runs the Waveshare record-then-play loopback indefinitely. */
void core1_audio_run_loopback(void);

#ifdef __cplusplus
}
#endif

#endif
