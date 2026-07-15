#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_DIR="$(cd "${ROOT_DIR}/.." && pwd)"
BOARD="${BOARD:-rpi_pico2/rp2350a/m33/w}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/pico2w_pico_clip}"
CACHE_DIR="${CACHE_DIR:-${ROOT_DIR}/build/zephyr-cache}"
export CCACHE_DIR="${CCACHE_DIR:-${ROOT_DIR}/build/ccache}"

if [ -f "${WORKSPACE_DIR}/walkie_talkie/.venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "${WORKSPACE_DIR}/walkie_talkie/.venv/bin/activate"
fi

if [ -x "${WORKSPACE_DIR}/walkie_talkie/.venv/bin/python" ]; then
  WEST_CMD=("${WORKSPACE_DIR}/walkie_talkie/.venv/bin/python" -m west)
else
  WEST_CMD=(west)
fi

"${WEST_CMD[@]}" build -p always -b "${BOARD}" "${ROOT_DIR}" -d "${BUILD_DIR}" -- -DUSER_CACHE_DIR="${CACHE_DIR}"
echo "UF2: ${BUILD_DIR}/zephyr/zephyr.uf2"
