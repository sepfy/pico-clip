#!/usr/bin/env bash
set -euo pipefail

app_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
toolchain_dir="${PICO_TOOLCHAIN_PATH:-/home/user/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin}"
test_mode="${CORE1_TEST_MODE:-loopback}"

case "${test_mode}" in
    loopback|birthday|benchmark) ;;
    *) echo "CORE1_TEST_MODE must be loopback, birthday, or benchmark" >&2; exit 2 ;;
esac

build_dir="${app_dir}/build_${test_mode}"

cmake -S "${app_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPICO_TOOLCHAIN_PATH="${toolchain_dir}" \
    -DPICO_GCC_TRIPLE=arm-zephyr-eabi \
    -DCORE1_TEST_MODE="${test_mode}"
cmake --build "${build_dir}" -j4
