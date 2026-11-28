# Zone 3: platforms. Thin per-target glue; no control logic, no physics.

# Plain C host: headless, free-running closed loop. Builds anywhere.
add_executable(open-crank-run host/run_main.c)
target_link_libraries(open-crank-run PRIVATE opencrank_sim)

# The acceptance suite on the in-memory loop, with the reference controller. Builds anywhere.
add_executable(open-crank-l2-run host/l2_run_main.c)
target_link_libraries(open-crank-l2-run PRIVATE opencrank_sim opencrank_accept m)

add_executable(open-crank-accept-loop host/accept_loop_main.c)
target_link_libraries(open-crank-accept-loop PRIVATE opencrank_sim opencrank_accept)

# Linux SocketCAN: the SIL executables, paced by the wall clock.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(open-crank-sil linux-socketcan/sil_main.c linux-socketcan/frames_sock.c)
    target_link_libraries(open-crank-sil PRIVATE opencrank_sim opentick::host)

    add_executable(open-crank-mc linux-socketcan/mc_main.c)
    target_link_libraries(open-crank-mc PRIVATE opencrank_portable opencrank_can_otk opentick::host)

    add_executable(open-crank-accept linux-socketcan/accept_main.c linux-socketcan/frames_sock.c)
    target_link_libraries(open-crank-accept PRIVATE opencrank_sim opencrank_accept opentick::host)

    # Level 2 on the board without a plant: where the ECU's injections land at a constant speed.
    add_executable(open-crank-ecu-check linux-socketcan/ecu_check_main.c)
    target_link_libraries(open-crank-ecu-check PRIVATE opencrank_sim opentick::host m)

    # Level 2 in a closed loop: the plant and the machine controller against a real engine ECU, through a bridge.
    add_executable(open-crank-hil linux-socketcan/hil_main.c)
    target_link_libraries(open-crank-hil PRIVATE opencrank_sim opencrank_accept opentick::host m)

    # Executables land next to each other in the top of the build tree.
    set_target_properties(open-crank-sil open-crank-mc open-crank-accept open-crank-ecu-check open-crank-hil PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
set_target_properties(open-crank-run open-crank-l2-run open-crank-accept-loop PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
