#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-rpi_pico2/rp2350a/m33/w}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/pico2w_pico_clip}"
CACHE_DIR="${CACHE_DIR:-${ROOT_DIR}/build/zephyr-cache}"
VENV_DIR="${VENV_DIR:-${ROOT_DIR}/.venv}"
export CCACHE_DIR="${CCACHE_DIR:-${ROOT_DIR}/build/ccache}"

if [ ! -x "${VENV_DIR}/bin/python" ]; then
  echo "Missing pico-clip Python environment: ${VENV_DIR}" >&2
  echo "Create it with: python3 -m venv ${VENV_DIR}" >&2
  exit 1
fi

WEST_CMD=("${VENV_DIR}/bin/python" -m west)
CMAKE_ARGS=(-DUSER_CACHE_DIR="${CACHE_DIR}" -DEXTRA_CONF_FILE=)

case "${1:-make}" in
  config)
    "${WEST_CMD[@]}" build -p auto -b "${BOARD}" "${ROOT_DIR}" -d "${BUILD_DIR}" \
      -t menuconfig -- "${CMAKE_ARGS[@]}"
    ;;
  make)
    "${WEST_CMD[@]}" build -p auto -b "${BOARD}" "${ROOT_DIR}" -d "${BUILD_DIR}" \
      -- "${CMAKE_ARGS[@]}"
    echo "UF2: ${BUILD_DIR}/zephyr/zephyr.uf2"
    ;;
  *)
    echo "Usage: $0 [config|make]" >&2
    exit 1
    ;;
esac
