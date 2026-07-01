# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/modules/modules.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pico_clip)

option(WIFI_SHELL_AMP_CPU0 "Build wifi_shell as the AMP CPU0 app" OFF)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(OPUS_HARDENING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/lib/opus ${CMAKE_CURRENT_BINARY_DIR}/opus)
target_compile_options(opus PRIVATE
  -mcpu=cortex-m33
  -mthumb
  -mabi=aapcs
  -mfp16-format=ieee
  -mtp=soft
)

zephyr_compile_options(-Wno-error=unused-parameter -Wno-error=unused-variable)
zephyr_compile_definitions(
  AIROC_WIFI_RX_PACKET_POOL_COUNT=32
  AIROC_WIFI_TX_PACKET_POOL_COUNT=16
  CONFIG_DTLS_USE_ECDSA=1
  CONFIG_DATA_BUFFER_SIZE=512
  CONFIG_AUDIO_BUFFER_SIZE=2048
  CONFIG_HTTP_BUFFER_SIZE=1024
  CONFIG_SDP_BUFFER_SIZE=4096
  CONFIG_MAX_NALU_SIZE=8192
  CONFIG_TLS_READ_TIMEOUT=10
  CONFIG_KEEPALIVE_TIMEOUT=30000
)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
zephyr_include_directories(${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_dma/include)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../RP2350-LCD-0.85/examples/C/02_ES8311/lib/audio_pio)

FILE(GLOB app_sources src/*.c)
list(REMOVE_ITEM app_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/pio_i2s.c)
target_sources(app PRIVATE ${app_sources})
target_sources(app PRIVATE ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common/hardware_dma/dma.c)
target_link_libraries(app PRIVATE opus)
