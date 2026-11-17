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

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(OC_WERROR)
    add_compile_options(-Werror)
endif()

# Dependencies point one way: platforms -> sim -> portable. See ARCHITECTURE.md.
add_subdirectory(portable)
add_subdirectory(sim)
add_subdirectory(platforms)

enable_testing()
add_subdirectory(tests)
