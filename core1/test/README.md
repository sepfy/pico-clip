# Core1 audio standalone test

Standalone RP2350/Pico SDK firmware that builds the audio implementation from
`../src` and runs the original Waveshare `Loopback_Test()` example.
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
