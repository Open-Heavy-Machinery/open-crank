# Firmware for the reference board (NUCLEO-G431KB), without CubeIDE.
# A project of its own, cross-compiled; the host build never enters here.
#
#   cmake -S platforms/stm32g4 -B build-stm32g4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build-stm32g4
#
# or simply tools/board/flash.sh, which also programs the board. Outputs:
# oc-stm32g4-demo.elf/.bin/.map (the reference controller) and
# oc-stm32g4-echo.elf/.bin/.map (the round-trip echo).
#
# ST's CMSIS and HAL are downloaded at configure time from STMicroelectronics'
# repositories, pinned by tag and SHA-256, and never copied into this
# repository (README.md, "Where each piece comes from").
cmake_minimum_required(VERSION 3.20)
project(open-crank-stm32g4 C ASM)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross-compile only: add -DCMAKE_TOOLCHAIN_FILE=<open-crank>/cmake/arm-none-eabi.cmake")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    # -O0 -g, as CubeIDE's Debug default: the execution times in README.md were measured so.
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
option(OC_WERROR "Treat warnings as errors (open-crank's code only, not ST's)" OFF)

# ---- ST's CMSIS and HAL, fetched and pinned ---------------------------------
include(FetchContent)
set(OC_ST_URL https://github.com/STMicroelectronics)
# SOURCE_SUBDIR names a directory without CMakeLists.txt: download only.
FetchContent_Declare(st_cmsis_core
    URL ${OC_ST_URL}/cmsis_core/archive/refs/tags/v5.9.0_20250520.tar.gz
    URL_HASH SHA256=da3e50301da157f322dba1d63bab3851c7ac8d793e3b13d7f621270d130aebd3
    SOURCE_SUBDIR none)
FetchContent_Declare(st_cmsis_device_g4
    URL ${OC_ST_URL}/cmsis_device_g4/archive/refs/tags/v1.2.6.tar.gz
    URL_HASH SHA256=a150d4b334610d8d8445385e9104d08aa7ae8c4e443b2a7dc6f19c9711d8bb4c
    SOURCE_SUBDIR none)
FetchContent_Declare(st_hal_g4
    URL ${OC_ST_URL}/stm32g4xx_hal_driver/archive/refs/tags/v1.2.7.tar.gz
    URL_HASH SHA256=a1129cebc41e1edda1ba81cd3a43d91fcc8b508a1c318cde9f9f136aba50909f
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(st_cmsis_core st_cmsis_device_g4 st_hal_g4)

set(OC_BOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/board)
set(OC_MCU_DEFS STM32G431xx USE_HAL_DRIVER)

# ST's code: built as it comes, its warnings are not ours to fix.
add_library(st_hal_g4 STATIC
    ${st_cmsis_device_g4_SOURCE_DIR}/Source/Templates/system_stm32g4xx.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_cortex.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_fdcan.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_flash.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_flash_ex.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_gpio.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_pwr.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_pwr_ex.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_rcc.c
    ${st_hal_g4_SOURCE_DIR}/Src/stm32g4xx_hal_rcc_ex.c
)
target_compile_definitions(st_hal_g4 PUBLIC ${OC_MCU_DEFS})
# SYSTEM: warnings from ST's headers do not reach open-crank's code.
target_include_directories(st_hal_g4 SYSTEM PUBLIC
    ${st_cmsis_core_SOURCE_DIR}/Include
    ${st_cmsis_device_g4_SOURCE_DIR}/Include
    ${st_hal_g4_SOURCE_DIR}/Inc
    ${OC_BOARD_DIR}   # stm32g4xx_hal_conf.h
)

# ---- open-crank's code ------------------------------------------------------
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../portable ${CMAKE_BINARY_DIR}/portable)

set(OC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion $<$<BOOL:${OC_WERROR}>:-Werror>)

# Board glue plus ST's start-up file (vector table, Reset_Handler), as objects
# so the linker keeps the vector table.
add_library(oc_board OBJECT
    ${OC_BOARD_DIR}/board.c
    ${OC_BOARD_DIR}/watchdog.c
    ${st_cmsis_device_g4_SOURCE_DIR}/Source/Templates/gcc/startup_stm32g431xx.s
)
target_link_libraries(oc_board PUBLIC st_hal_g4)
target_compile_options(oc_board PRIVATE $<$<COMPILE_LANGUAGE:C>:${OC_WARNINGS}>)

function(oc_firmware name main)
    add_executable(${name} ${main} $<TARGET_OBJECTS:oc_board>)
    set_target_properties(${name} PROPERTIES SUFFIX .elf)
    target_link_libraries(${name} PRIVATE oc_board st_hal_g4 ${ARGN})
    target_compile_options(${name} PRIVATE ${OC_WARNINGS})
    target_link_options(${name} PRIVATE
        -T${OC_BOARD_DIR}/stm32g431kb.ld
        --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,--print-memory-usage
        -Wl,-Map=${CMAKE_BINARY_DIR}/${name}.map)
    set_target_properties(${name} PROPERTIES LINK_DEPENDS ${OC_BOARD_DIR}/stm32g431kb.ld)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${CMAKE_BINARY_DIR}/${name}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>
        VERBATIM)
endfunction()

oc_firmware(oc-stm32g4-demo ${CMAKE_CURRENT_SOURCE_DIR}/demo/main.c opencrank_portable)
# Test build only: the demo plus an intentional hang on the test frame, to test
# the watchdog on the bus (tools/board/watchdog_check.sh). Never the reference.
oc_firmware(oc-stm32g4-demo-hangtest ${CMAKE_CURRENT_SOURCE_DIR}/demo/main.c opencrank_portable)
target_compile_definitions(oc-stm32g4-demo-hangtest PRIVATE OC_WDG_HANG_TEST=1)
oc_firmware(oc-stm32g4-echo ${CMAKE_CURRENT_SOURCE_DIR}/echo/main.c)
