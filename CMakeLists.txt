cmake_minimum_required(VERSION 3.20)

# Project definition
project(biascontrol C ASM)

# STM32H523CET6 definitions
add_compile_definitions(
    STM32H523xx
    USE_HAL_DRIVER
    HSE_VALUE=8192000
)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Compiler options
add_compile_options(
    -Wall -Wextra -Wshadow
    -fdata-sections -ffunction-sections
    -fno-common
)

# Debug/Release flags (set via CMAKE_C_FLAGS_DEBUG/RELEASE)
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g3" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2 -g"  CACHE STRING "" FORCE)

# --------------------------------------------------------------------------
# CubeMX generated code (do NOT edit files under cubemx/ manually)
# --------------------------------------------------------------------------
set(CUBEMX_DIR ${CMAKE_SOURCE_DIR}/cubemx)

# Linker script (CubeMX generated, heap=0x800, stack=0x1000)
# Guard ARM-specific flags so they are not applied to host-side unit tests.
if(NOT BUILD_TESTS)
    add_link_options(
        -Wl,--gc-sections
        -Wl,--print-memory-usage
        -T${CUBEMX_DIR}/STM32H523xx_FLASH.ld
    )
endif()

# Startup assembly
set(STARTUP_SRC ${CUBEMX_DIR}/startup_stm32h523xx.s)

# STM32 HAL library sources (exclude template files)
file(GLOB HAL_SRC ${CUBEMX_DIR}/Drivers/STM32H5xx_HAL_Driver/Src/*.c)
list(FILTER HAL_SRC EXCLUDE REGEX "_template\\.c$")

# CubeMX Core sources (main.c, gpio.c, spi.c, gpdma.c, tim.c, usart.c, etc.)
file(GLOB CORE_SRC ${CUBEMX_DIR}/Core/Src/*.c)

# --------------------------------------------------------------------------
# Source files by layer
# --------------------------------------------------------------------------

# Driver layer (custom IC drivers: ADS131M02, DAC8568, board GPIO)
file(GLOB DRIVER_SRC src/drivers/*.c)

# DSP layer (pure math, no hardware dependencies)
file(GLOB DSP_SRC src/dsp/*.c)

# Control layer (bias loop, modulator strategies, PID)
file(GLOB CTRL_SRC src/control/*.c)

# Application layer (state machine, config)
file(GLOB APP_SRC src/app/*.c)

# --------------------------------------------------------------------------
# Executable (firmware target — skipped when BUILD_TESTS=ON, since the host
# compiler cannot assemble the ARM startup file)
# --------------------------------------------------------------------------
if(NOT BUILD_TESTS)
    add_executable(${PROJECT_NAME}.elf
        ${STARTUP_SRC}
        ${HAL_SRC}
        ${CORE_SRC}
        ${DRIVER_SRC}
        ${DSP_SRC}
        ${CTRL_SRC}
        ${APP_SRC}
    )

    # Link math library; enable float support in newlib-nano printf/scanf
    target_link_libraries(${PROJECT_NAME}.elf m)
    target_link_options(${PROJECT_NAME}.elf PRIVATE -u _printf_float)

    target_include_directories(${PROJECT_NAME}.elf PRIVATE
        # CubeMX generated headers
        ${CUBEMX_DIR}/Core/Inc
        ${CUBEMX_DIR}/Drivers/STM32H5xx_HAL_Driver/Inc
        ${CUBEMX_DIR}/Drivers/CMSIS/Device/ST/STM32H5xx/Include
        ${CUBEMX_DIR}/Drivers/CMSIS/Include
        # Project layers
        src/drivers
        src/dsp
        src/control
        src/app
    )

    # ----------------------------------------------------------------------
    # Post-build: generate .hex and .bin, print size
    # ----------------------------------------------------------------------
    add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE}    $<TARGET_FILE:${PROJECT_NAME}.elf>
        COMMENT "Generating HEX/BIN and printing size"
    )
endif()

# --------------------------------------------------------------------------
# Host-side test target (native build, not cross-compiled)
# --------------------------------------------------------------------------
# Build with: cmake -DBUILD_TESTS=ON .. (using native compiler, not cross)
option(BUILD_TESTS "Build host-side unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_goertzel
        test/test_goertzel.c
        src/dsp/dsp_goertzel.c
    )
    target_include_directories(test_goertzel PRIVATE src/dsp)
    target_link_libraries(test_goertzel m)
    add_test(NAME goertzel COMMAND test_goertzel)

    add_executable(test_pid
        test/test_pid.c
        src/control/ctrl_pid.c
    )
    target_include_directories(test_pid PRIVATE src/control)
    target_link_libraries(test_pid m)
    add_test(NAME pid COMMAND test_pid)

    add_executable(test_mzm_phase
        test/test_mzm_phase.c
        src/control/ctrl_modulator_mzm.c
    )
    target_include_directories(test_mzm_phase PRIVATE src/control src/dsp)
    target_link_libraries(test_mzm_phase m)
    add_test(NAME mzm_phase COMMAND test_mzm_phase)
endif()
