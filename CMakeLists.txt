# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/modules/modules.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pico_clip)

option(WIFI_SHELL_AMP_CPU0 "Build wifi_shell as the AMP CPU0 app" OFF)

zephyr_compile_options(-Wno-error=unused-parameter -Wno-error=unused-variable)
zephyr_compile_definitions(
  AIROC_WIFI_RX_PACKET_POOL_COUNT=32
  AIROC_WIFI_TX_PACKET_POOL_COUNT=16
  CONFIG_DTLS_USE_ECDSA=1
  CONFIG_DATA_BUFFER_SIZE=512
  CONFIG_AUDIO_BUFFER_SIZE=2048
  CONFIG_HTTP_BUFFER_SIZE=2048
  CONFIG_SDP_BUFFER_SIZE=4096
  CONFIG_MAX_NALU_SIZE=8192
  CONFIG_TLS_READ_TIMEOUT=10000
  CONFIG_KEEPALIVE_TIMEOUT=30000
)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_dma/include)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core1/src)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core1/src/audio_pio)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core1/src/config)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core1/src/es8311)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/common/pico_stdlib_headers/include)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_i2c/include)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_spi/include)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_pwm/include)
zephyr_compile_definitions(PICO_CLIP_ZEPHYR_CORE1=1)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_sources(app PRIVATE ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_dma/dma.c)
target_sources(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core1/src/core1_audio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/core1/src/core1_pcmu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/core1/src/audio_pio/audio_pio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/core1/src/config/DEV_Config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/core1/src/es8311/es8311.c
)
