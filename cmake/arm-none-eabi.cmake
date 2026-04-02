set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ARM GNU Toolchain path — auto-detect across macOS, Linux, and Windows (MSYS2/winget)
find_program(ARM_GCC arm-none-eabi-gcc
    PATHS
        # macOS (Homebrew / ARM installer)
        /Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
        /Applications/ArmGNUToolchain/*/arm-none-eabi/bin
        /opt/homebrew/bin
        /usr/local/bin
        # Windows — MSYS2 mingw64
        C:/msys64/mingw64/bin
        # Windows — ARM GNU Toolchain installer default paths
        "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/13.3 rel1/bin"
        "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/12.3 rel1/bin"
        "C:/Program Files/Arm GNU Toolchain arm-none-eabi/13.3 rel1/bin"
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

# On Windows the executables have a .exe suffix; append it when present
if(WIN32)
    set(EXE_SUFFIX ".exe")
else()
    set(EXE_SUFFIX "")
endif()

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc${EXE_SUFFIX})
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc${EXE_SUFFIX})
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++${EXE_SUFFIX})
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy${EXE_SUFFIX})
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump${EXE_SUFFIX})
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size${EXE_SUFFIX})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU flags for STM32H523CET6 (Cortex-M33, FPv5-SP FPU)
set(MCU_FLAGS "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")

set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} --specs=nano.specs -lnosys")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
