# Core1 audio implementation

Audio implementation under `core1/src`, shared by the standalone `core1/test`
firmware and the Zephyr-managed Core1 worker.

The public entry points are declared in `core1_audio.h`. Zephyr calls
`core1_audio_run_pcmu_stream()` for 8 kHz, 20 ms, full-duplex PCMU audio. The
standalone firmware calls the same initialization and PIO implementation for
raw loopback and the retained birthday test.

The ES8311, board configuration, PIO sources, and Happy Birthday PCM data are
taken from Waveshare's RP2350-LCD-0.85 `02_ES8311` example. Unused Waveshare
playback helpers and the larger music asset are omitted.

Directory layout:

- `audio_pio`: PIO programs and DMA-based capture/playback/loopback
- `es8311`: ES8311 codec initialization and controls
- `config`: board GPIO, I2C, DMA, and power initialization
- `audio_data`: Happy Birthday PCM data for the standalone test
