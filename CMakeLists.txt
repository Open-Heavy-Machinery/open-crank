cmake_minimum_required(VERSION 3.20)
project(open-crank C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Plain ISO C11, no GNU extensions. Among other things this keeps GCC from
# fusing a*b+c into FMA instructions behind our back (-ffp-contract=off is
# the ISO-mode default), so host and target results differ only for real reasons.
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OC_WERROR "Treat warnings as errors" OFF)

# open-tick: the machine-independent core (timebase, scenarios, trace, CAN
# bridges, drivers), pinned as a git submodule. Added before open-crank's
# warning flags so they do not leak into it; it gets the same -Werror choice.
set(OC_OPEN_TICK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/open-tick)
if(NOT EXISTS ${OC_OPEN_TICK_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "open-tick is missing from external/open-tick. Run: git submodule update --init")
endif()
set(OTK_WERROR ${OC_WERROR})
add_subdirectory(${OC_OPEN_TICK_DIR} ${CMAKE_BINARY_DIR}/open-tick)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(OC_WERROR)
    add_compile_options(-Werror)
endif()

# Dependencies point one way: platforms -> sim -> portable, and sim and
# platforms may use open-tick; portable never does. See ARCHITECTURE.md.
add_subdirectory(portable)
add_subdirectory(sim)
add_subdirectory(platforms)

enable_testing()
add_subdirectory(tests)
