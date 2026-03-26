set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ARM GNU Toolchain path (installed via 'brew install --cask gcc-arm-embedded')
# Auto-detect: search well-known locations, fall back to bare names (PATH lookup)
find_program(ARM_GCC arm-none-eabi-gcc
    PATHS
        /Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
        /Applications/ArmGNUToolchain/*/arm-none-eabi/bin
        /opt/homebrew/bin
        /usr/local/bin
    NO_DEFAULT_PATH
)
if(NOT ARM_GCC)
    find_program(ARM_GCC arm-none-eabi-gcc)
endif()
if(ARM_GCC)
    get_filename_component(TOOLCHAIN_DIR ${ARM_GCC} DIRECTORY)
    set(TOOLCHAIN_PREFIX ${TOOLCHAIN_DIR}/arm-none-eabi-)
else()
    set(TOOLCHAIN_PREFIX arm-none-eabi-)
endif()

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump)
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU flags for STM32H523CET6 (Cortex-M33, FPv5-SP FPU)
set(MCU_FLAGS "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")

set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} --specs=nano.specs -lm -lnosys")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
