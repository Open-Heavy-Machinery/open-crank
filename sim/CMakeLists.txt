# Zone 2: sim. Host-only simulator: plant, loads, engine ECU, runtime.
# May depend on portable and open-tick; never on platforms.

# J1939 frames to and from open-tick's CAN frames. A library of its own so
# that a device-under-test program (open-crank-mc) can use open-tick's CAN
# bridge without linking the simulator.
add_library(opencrank_can_otk STATIC can_otk/src/can_otk.c)
target_include_directories(opencrank_can_otk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/can_otk/include)
target_link_libraries(opencrank_can_otk PUBLIC opencrank_portable opentick::core)

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
target_link_libraries(opencrank_sim PUBLIC opencrank_portable opencrank_can_otk opentick::core m)

# Acceptance oracle. Only opentick::core and its own include dir: an open-crank #include in it must not compile.
add_library(opencrank_oracle STATIC oracle/src/oracle.c)
target_include_directories(opencrank_oracle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/oracle/include)
target_link_libraries(opencrank_oracle PUBLIC opentick::core)

# open-crank's observables for the oracle: what the device under test sends.
add_library(opencrank_accept STATIC accept/src/accept.c)
target_include_directories(opencrank_accept PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/accept/include)
target_link_libraries(opencrank_accept PUBLIC opencrank_oracle opencrank_can_otk opencrank_portable)
