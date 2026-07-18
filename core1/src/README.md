# Core1 audio implementation

Audio implementation under `core1/src`, shared by the standalone `core1/test`
firmware and the future AMP Core1 integration.

The public entry points are declared in `core1_audio.h`. The standalone test
and a future Zephyr-managed Core1 worker explicitly call `core1_audio_init()`
and then `core1_audio_run_loopback()` without importing anything from
`core1/test`.

The ES8311, board configuration, PIO sources, and Happy Birthday PCM data are
taken from Waveshare's RP2350-LCD-0.85 `02_ES8311` example. The much larger
music asset is omitted; `music.h` only retains declarations needed to compile
the unused `Music_Out()` function.

Directory layout:

- `audio_pio`: PIO programs and DMA-based capture/playback/loopback
- `es8311`: ES8311 codec initialization and controls
- `config`: board GPIO, I2C, DMA, and power initialization
- `audio_data`: declarations needed to compile unused playback functions
