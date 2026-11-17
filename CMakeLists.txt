cmake_minimum_required(VERSION 3.16)
project(open-crank C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic)

# ------------------------------------------------------------------------------
# Zone 1: portable (goes to firmware)
# ------------------------------------------------------------------------------
add_library(opencrank_portable STATIC
    portable/j1939/src/j1939.c
)
target_include_directories(opencrank_portable PUBLIC
    portable/j1939/include
    sim/core/include  # TEMPORARY: the codec still uses PlantState; removed with the standard TSC1 codec
)

# ------------------------------------------------------------------------------
# Zone 2: sim (host only)
# ------------------------------------------------------------------------------
add_library(opencrank_sim STATIC
    sim/core/src/plant.c
    sim/plants/d936la6/src/d936la6.c
    sim/plants/d936la6/src/d936la6_ecu.c
    sim/loads/src/hydraulic_pump.c
    sim/engine_ecu/src/engine_ecu.c
)
target_include_directories(opencrank_sim PUBLIC
    sim/core/include
    sim/plants/d936la6/include
    sim/loads/include
    sim/engine_ecu/include
)
target_link_libraries(opencrank_sim PUBLIC opencrank_portable m)

# ------------------------------------------------------------------------------
# Zone 3: platforms
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
# Tests
# ------------------------------------------------------------------------------
enable_testing()
foreach(test_name test_plant test_hydraulic_pump test_engine_ecu)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE opencrank_sim m)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
