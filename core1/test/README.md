# Core1 audio standalone tests

This directory builds standalone RP2350 Pico SDK firmware using the audio
implementations from `../src`.

```sh
./scripts/build.sh
CORE1_TEST_MODE=birthday ./scripts/build.sh
CORE1_TEST_MODE=benchmark ./scripts/build.sh
```

Modes:

- `loopback`: Waveshare raw record-then-play DMA loopback.
- `birthday`: repeating Waveshare Happy Birthday playback.
- `benchmark`: isolated fixed-point Opus timing benchmark on UART0 GP16/GP17.

The loopback and birthday builds keep diagnostic `printf` disabled because the
default UART GPIO0/1 overlap the audio interface.
