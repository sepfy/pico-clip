cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../modules/hal/rpi_pico" CACHE PATH "Pico SDK path")
set(PICO_BOARD pico2 CACHE STRING "Board type")
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(core1_test C CXX ASM)
pico_sdk_init()

set(CORE1_TEST_MODE "loopback" CACHE STRING "Audio test: loopback, opus, or birthday")
set_property(CACHE CORE1_TEST_MODE PROPERTY STRINGS loopback opus birthday)
if(NOT CORE1_TEST_MODE STREQUAL "loopback" AND
   NOT CORE1_TEST_MODE STREQUAL "opus" AND
   NOT CORE1_TEST_MODE STREQUAL "birthday")
    message(FATAL_ERROR "CORE1_TEST_MODE must be loopback, opus, or birthday")
endif()

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
set(OPUS_VAR_ARRAYS OFF CACHE BOOL "" FORCE)
set(OPUS_USE_ALLOCA OFF CACHE BOOL "" FORCE)
set(OPUS_NONTHREADSAFE_PSEUDOSTACK ON CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(OPUS_HARDENING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../modules/lib/opus
                 ${CMAKE_CURRENT_BINARY_DIR}/opus)

set(CORE1_AUDIO_SOURCES
    ../src/core1_audio.c
    ../src/core1_opus.c
    ../src/audio_pio/audio_pio.c
    ../src/audio_data/audio_data.c
    ../src/config/DEV_Config.c
    ../src/es8311/es8311.c
)

set_source_files_properties(
    src/main.c
    ${CORE1_AUDIO_SOURCES}
    PROPERTIES
    COMPILE_DEFINITIONS "printf=audio_noop_printf"
)

function(add_core1_audio_test target entry_source)
    add_executable(${target} ${entry_source} ${CORE1_AUDIO_SOURCES})
    pico_generate_pio_header(${target}
        ${CMAKE_CURRENT_LIST_DIR}/../src/audio_pio/audio_pio.pio
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/../src
        ${CMAKE_CURRENT_LIST_DIR}/../src/audio_pio
        ${CMAKE_CURRENT_LIST_DIR}/../src/audio_data
        ${CMAKE_CURRENT_LIST_DIR}/../src/config
        ${CMAKE_CURRENT_LIST_DIR}/../src/es8311
    )
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-include;${CMAKE_CURRENT_LIST_DIR}/include/toolchain_compat.h>
    )
    target_link_libraries(${target} PRIVATE
        pico_stdlib
        hardware_adc
        hardware_clocks
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_pwm
        hardware_spi
        opus
    )
    pico_enable_stdio_usb(${target} 0)
    pico_enable_stdio_uart(${target} 0)
    pico_set_printf_implementation(${target} none)
    pico_add_extra_outputs(${target})
endfunction()

add_core1_audio_test(core1_test src/main.c)
if(CORE1_TEST_MODE STREQUAL "birthday")
    target_compile_definitions(core1_test PRIVATE CORE1_TEST_BIRTHDAY=1)
elseif(CORE1_TEST_MODE STREQUAL "opus")
    target_compile_definitions(core1_test PRIVATE CORE1_TEST_OPUS=1)
endif()
