# Zone 2: sim. Host-only simulator: plant, loads, engine ECU, runtime.
# May depend on portable; never on platforms.
add_library(opencrank_sim STATIC
    core/src/plant.c
    plants/d936la6/src/d936la6.c
    plants/d936la6/src/d936la6_ecu.c
    loads/src/hydraulic_pump.c
    engine_ecu/src/engine_ecu.c
    runtime/src/bench.c
    runtime/src/signals.c
    runtime/src/scenario.c
    runtime/src/loop.c
    runtime/src/trace.c
)
target_include_directories(opencrank_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/plants/d936la6/include
    ${CMAKE_CURRENT_SOURCE_DIR}/loads/include
    ${CMAKE_CURRENT_SOURCE_DIR}/engine_ecu/include
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
)
target_link_libraries(opencrank_sim PUBLIC opencrank_portable m)
