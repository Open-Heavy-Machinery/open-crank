# Toolchain file: Arm Cortex-M4F (STM32G4 class), bare metal, hard-float ABI.
#
#   cmake -S portable -B build-m4f -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake -S platforms/stm32g4 -B build-stm32g4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# The compiler is taken from PATH; tools/board/setup.sh installs the pinned
# version (tools/board/arm_toolchain.sh) and the board scripts put it first.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)

# No OS to link against: only check that the compiler can build a library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -ffreestanding -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
