cmake_minimum_required(VERSION 3.16)
project(open-crank C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

# ------------------------------------------------------------------------------
# Zone 1: portable (goes to firmware). Depends on nothing else in the repo.
# ------------------------------------------------------------------------------
add_library(opencrank_portable STATIC
    portable/j1939/src/j1939.c
    portable/j1939/src/oc_msgs.c
    portable/machine_ctrl/src/machine_ctrl.c
)
target_include_directories(opencrank_portable PUBLIC
    portable/j1939/include
    portable/machine_ctrl/include
)
target_compile_options(opencrank_portable PRIVATE -Wconversion -Wdouble-promotion)

# ------------------------------------------------------------------------------
# Zone 2: sim (host only). May depend on portable.
# ------------------------------------------------------------------------------
add_library(opencrank_sim STATIC
    sim/core/src/plant.c
    sim/plants/d936la6/src/d936la6.c
    sim/plants/d936la6/src/d936la6_ecu.c
    sim/loads/src/hydraulic_pump.c
    sim/engine_ecu/src/engine_ecu.c
    sim/runtime/src/bench.c
)
target_include_directories(opencrank_sim PUBLIC
    sim/core/include
    sim/plants/d936la6/include
    sim/loads/include
    sim/engine_ecu/include
    sim/runtime/include
)
target_link_libraries(opencrank_sim PUBLIC opencrank_portable m)

# ------------------------------------------------------------------------------
# Zone 3: platforms
# ------------------------------------------------------------------------------
add_library(opencrank_socketcan STATIC
    platforms/linux-socketcan/socketcan.c
    platforms/linux-socketcan/pacer.c
)
target_include_directories(opencrank_socketcan PUBLIC platforms/linux-socketcan)
target_link_libraries(opencrank_socketcan PUBLIC opencrank_portable)

add_executable(open-crank-sil platforms/linux-socketcan/sil_main.c)
target_link_libraries(open-crank-sil PRIVATE opencrank_sim opencrank_socketcan)

add_executable(open-crank-mc platforms/linux-socketcan/mc_main.c)
target_link_libraries(open-crank-mc PRIVATE opencrank_portable opencrank_socketcan)

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
enable_testing()
foreach(test_name test_j1939 test_machine_ctrl test_plant test_hydraulic_pump test_engine_ecu test_bench)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE opencrank_sim m)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
