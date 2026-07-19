# Core1 audio standalone test

Standalone RP2350/Pico SDK firmware that builds the audio implementation from
`../src` and runs the Waveshare audio tests or the 20 ms Opus loopback.

Build modes:

```sh
./scripts/build.sh
CORE1_TEST_MODE=opus ./scripts/build.sh
CORE1_TEST_MODE=birthday ./scripts/build.sh
```

The Opus mode captures 480 mono samples at 24 kHz with ping-pong RX DMA and
encodes each frame outside the DMA IRQ. It buffers 25 encoded 20 ms packets to
produce a 500 ms loopback delay, then decodes into ping-pong TX DMA buffers. It
uses fixed-point Opus at 24 kbit/s and complexity 0.
This directory contains only the test entry point and build support needed to
produce a firmware image before the audio code is integrated into AMP Core1.

Build:

```sh
./scripts/build.sh
CORE1_TEST_MODE=birthday ./scripts/build.sh
```

The single `src/main.c` selects its behavior with the `CORE1_TEST_BIRTHDAY`
compile-time macro. The build script sets it through
`CORE1_TEST_MODE=loopback|birthday`.

Firmware outputs:

- `build_loopback/core1_test.uf2`: original record-then-play loopback
- `build_birthday/core1_test.uf2`: original repeating Happy Birthday playback

Diagnostic `printf` output is disabled because the workspace's Pico SDK
checkout does not include TinyUSB and GPIO0/1 used by the default UART are
occupied by the audio interface.
