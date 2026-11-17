# Zone 1: portable. Goes to firmware. Depends on nothing else in the repository,
# and builds on its own so that firmware projects (and the Cortex-M4F
# portability check) can consume it without the simulator:
#
#   cmake -S portable -B build-m4f -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(opencrank_portable C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    option(OC_WERROR "Treat warnings as errors" OFF)
endif()

add_library(opencrank_portable STATIC
    j1939/src/j1939.c
    j1939/src/oc_msgs.c
    machine_ctrl/src/machine_ctrl.c
)
target_include_directories(opencrank_portable PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/j1939/include
    ${CMAKE_CURRENT_SOURCE_DIR}/machine_ctrl/include
)
# Stricter than the rest of the tree: narrowing and float->double promotion
# are bugs on a single-precision FPU.
target_compile_options(opencrank_portable PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion
    $<$<BOOL:${OC_WERROR}>:-Werror>
)
