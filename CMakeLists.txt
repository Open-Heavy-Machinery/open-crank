cmake_minimum_required(VERSION 3.16)
project(open-crank C)

# Enforce strict C11 standard and enable comprehensive compiler warnings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

# ------------------------------------------------------------------------------
# Core Simulation Library (OS-independent: physics, engine models, J1939, loads)
# ------------------------------------------------------------------------------
add_library(opencrank_core STATIC
    core/src/plant.c
    plants/d936la6/src/d936la6.c
    j1939/src/j1939.c
    loads/src/hydraulic_pto.c
)

target_include_directories(opencrank_core PUBLIC
    core/include
    plants/d936la6/include
    j1939/include
    loads/include
)

# Link standard math library (libm)
target_link_libraries(opencrank_core PUBLIC m)

# ------------------------------------------------------------------------------
# Linux SocketCAN Software-in-the-Loop (SIL) Simulator Executable
# ------------------------------------------------------------------------------
add_executable(open-crank-sil
    backends/sil-vcan/main.c
)
target_link_libraries(open-crank-sil PRIVATE opencrank_core)

# ------------------------------------------------------------------------------
# Reference Electronic Control Unit (ECU) Firmware Executable
# ------------------------------------------------------------------------------
add_executable(open-crank-ecu
    ecu/reference-fw/main.c
)

# ------------------------------------------------------------------------------
# Automated Unit Tests (CTest)
# ------------------------------------------------------------------------------
enable_testing()
add_executable(test_d936 tests/test_d936.c)
target_link_libraries(test_d936 PRIVATE opencrank_core)
add_test(NAME d936_unit_test COMMAND test_d936)
