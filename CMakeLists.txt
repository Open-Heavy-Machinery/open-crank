cmake_minimum_required(VERSION 3.16)
project(open-crank C)

# Enforce strict C11 standard and enable comprehensive compiler warnings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

# ------------------------------------------------------------------------------
# Zone 1: Portable Library (Zero OS/simulator dependencies, MCU-ready)
# ------------------------------------------------------------------------------
add_library(opencrank_portable STATIC
    portable/j1939/src/j1939.c
)

target_include_directories(opencrank_portable PUBLIC
    portable/j1939/include
)

# ------------------------------------------------------------------------------
# Zone 2: Host Simulation Library (Physics, plants, loads -> depends on portable)
# ------------------------------------------------------------------------------
add_library(opencrank_sim STATIC
    sim/core/src/plant.c
    sim/plants/d936la6/src/d936la6.c
    sim/loads/src/hydraulic_pto.c
)

target_include_directories(opencrank_sim PUBLIC
    sim/core/include
    sim/plants/d936la6/include
    sim/loads/include
)
target_include_directories(opencrank_portable PUBLIC
    portable/j1939/include
    sim/core/include 
)

target_link_libraries(opencrank_sim PUBLIC opencrank_portable m)

# ------------------------------------------------------------------------------
# Zone 3: Linux SocketCAN Platform Executables
# ------------------------------------------------------------------------------
add_executable(open-crank-sil
    platforms/linux-socketcan/sil_main.c
    platforms/linux-socketcan/socketcan.c
)
target_link_libraries(open-crank-sil PRIVATE opencrank_sim)

add_executable(open-crank-mc
    platforms/linux-socketcan/mc_main.c
    platforms/linux-socketcan/socketcan.c
)
target_link_libraries(open-crank-mc PRIVATE opencrank_portable)

# ------------------------------------------------------------------------------
# Automated Unit Tests (CTest)
# ------------------------------------------------------------------------------
enable_testing()
add_executable(test_d936 tests/test_d936.c)
target_link_libraries(test_d936 PRIVATE opencrank_sim)
add_test(NAME d936_unit_test COMMAND test_d936)
