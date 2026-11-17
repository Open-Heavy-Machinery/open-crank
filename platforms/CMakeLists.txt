# Zone 3: platforms. Thin per-target glue; no control logic, no physics.

# Plain C host: headless, free-running closed loop. Builds anywhere.
add_executable(open-crank-run host/run_main.c)
target_link_libraries(open-crank-run PRIVATE opencrank_sim)

# Linux SocketCAN: the SIL executables, paced by the wall clock.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(opencrank_socketcan STATIC
        linux-socketcan/socketcan.c
        linux-socketcan/pacer.c
    )
    target_include_directories(opencrank_socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/linux-socketcan)
    target_link_libraries(opencrank_socketcan PUBLIC opencrank_portable)

    add_executable(open-crank-sil linux-socketcan/sil_main.c)
    target_link_libraries(open-crank-sil PRIVATE opencrank_sim opencrank_socketcan)

    add_executable(open-crank-mc linux-socketcan/mc_main.c)
    target_link_libraries(open-crank-mc PRIVATE opencrank_portable opencrank_socketcan)

    # Executables land next to each other in the top of the build tree.
    set_target_properties(open-crank-sil open-crank-mc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
set_target_properties(open-crank-run PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
