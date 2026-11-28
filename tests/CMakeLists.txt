# Unit and closed-loop tests. All headless and free-running: no sockets, no
# wall clock. They use CHECK (tests/check.h), never assert(), so Release
# builds still test.
set(OC_TESTS
    test_j1939
    test_machine_ctrl
    test_ecu
    test_plant
    test_plant_l2
    test_hydraulic_pump
    test_engine_ecu
    test_bench
    test_scenario
    test_closed_loop
    test_accept
)
foreach(test_name ${OC_TESTS})
    add_executable(${test_name} ${test_name}.c)
    target_link_libraries(${test_name} PRIVATE opencrank_sim m)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

target_link_libraries(test_accept PRIVATE opencrank_accept)

set(OC_SCENARIO_DIR ${PROJECT_SOURCE_DIR}/scenarios)
target_compile_definitions(test_closed_loop PRIVATE OC_SCENARIO_DIR="${OC_SCENARIO_DIR}")

# Every shipped scenario must parse, so a broken demo file fails the build.
file(GLOB OC_SCENARIO_FILES CONFIGURE_DEPENDS ${OC_SCENARIO_DIR}/*.csv ${OC_SCENARIO_DIR}/board/*.csv)
list(FILTER OC_SCENARIO_FILES EXCLUDE REGEX "\\.accept\\.csv$") # acceptance criteria, not scenarios
if(NOT OC_SCENARIO_FILES)
    message(FATAL_ERROR "no scenario files found in ${OC_SCENARIO_DIR}")
endif()
add_test(NAME test_scenario_files COMMAND test_scenario ${OC_SCENARIO_FILES})

# Smoke test of the headless runner, as a user would call it.
add_test(NAME run_hoist_step COMMAND open-crank-run ${OC_SCENARIO_DIR}/hoist_step.csv --quiet)

# The reference controller passes the acceptance suite in the in-memory loop:
# one test per scenarios/<name>.accept.csv, named accept_<name>. Each also writes
# its verdict file (open-tick docs/verdict-format.md) into the build tree.
file(GLOB OC_ACCEPT_FILES CONFIGURE_DEPENDS ${OC_SCENARIO_DIR}/*.accept.csv)
foreach(criteria ${OC_ACCEPT_FILES})
    get_filename_component(criteria_name ${criteria} NAME)
    string(REGEX REPLACE "\\.accept\\.csv$" "" scenario_name ${criteria_name})
    add_test(NAME accept_${scenario_name}
             COMMAND open-crank-accept-loop ${OC_SCENARIO_DIR}/${scenario_name}.csv
                     --verdicts ${CMAKE_CURRENT_BINARY_DIR}/${scenario_name}.verdicts.csv)
endforeach()

# hoist_step's verdict file is the one kept in tests/data, byte for byte.
set_tests_properties(accept_hoist_step PROPERTIES FIXTURES_SETUP hoist_step_verdicts)
add_test(NAME accept_verdicts_hoist_step
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_SOURCE_DIR}/data/hoist_step.verdicts.csv
                 ${CMAKE_CURRENT_BINARY_DIR}/hoist_step.verdicts.csv)
set_tests_properties(accept_verdicts_hoist_step PROPERTIES FIXTURES_REQUIRED hoist_step_verdicts)

# Frame logs (--frames, open-tick docs/frame-log-format.md): in memory, two runs
# of a fault scenario give the same file, byte for byte.
foreach(run a b)
    add_test(NAME accept_frames_${run}
             COMMAND open-crank-accept-loop ${OC_SCENARIO_DIR}/speed_command_corrupt.csv
                     --frames ${CMAKE_CURRENT_BINARY_DIR}/speed_command_corrupt_${run}.frames.csv)
    set_tests_properties(accept_frames_${run} PROPERTIES FIXTURES_SETUP frames_runs)
endforeach()
add_test(NAME accept_frames_deterministic
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/speed_command_corrupt_a.frames.csv
                 ${CMAKE_CURRENT_BINARY_DIR}/speed_command_corrupt_b.frames.csv)
set_tests_properties(accept_frames_deterministic PROPERTIES FIXTURES_REQUIRED frames_runs)

# open-crank-run --frames: the trace and the frame log come from the same run.
# Two runs give the same frame log; the trace is the same with and without
# --frames; and the frames are those of open-crank-accept-loop for the same
# scenario, byte for byte except the header's source.
set(OC_RUN_OUT ${CMAKE_CURRENT_BINARY_DIR}/run_hoist_step)
foreach(run a b)
    add_test(NAME run_frames_hoist_step_${run}
             COMMAND open-crank-run ${OC_SCENARIO_DIR}/hoist_step.csv --quiet
                     --trace ${OC_RUN_OUT}_${run}.trace.csv --frames ${OC_RUN_OUT}_${run}.frames.csv)
    set_tests_properties(run_frames_hoist_step_${run} PROPERTIES FIXTURES_SETUP run_frames)
endforeach()
add_test(NAME run_trace_hoist_step_no_frames
         COMMAND open-crank-run ${OC_SCENARIO_DIR}/hoist_step.csv --quiet --trace ${OC_RUN_OUT}_plain.trace.csv)
set_tests_properties(run_trace_hoist_step_no_frames PROPERTIES FIXTURES_SETUP run_frames)
add_test(NAME accept_frames_hoist_step
         COMMAND open-crank-accept-loop ${OC_SCENARIO_DIR}/hoist_step.csv
                 --frames ${CMAKE_CURRENT_BINARY_DIR}/accept_hoist_step.frames.csv)
set_tests_properties(accept_frames_hoist_step PROPERTIES FIXTURES_SETUP run_frames)

add_test(NAME run_frames_hoist_step
         COMMAND ${CMAKE_COMMAND} -E compare_files ${OC_RUN_OUT}_a.frames.csv ${OC_RUN_OUT}_b.frames.csv)
add_test(NAME run_trace_same_with_frames
         COMMAND ${CMAKE_COMMAND} -E compare_files ${OC_RUN_OUT}_plain.trace.csv ${OC_RUN_OUT}_a.trace.csv)
add_test(NAME run_frames_same_as_accept_loop
         COMMAND sh -c "sed '1s/; source=open-crank-run$/; source=open-crank-accept-loop/' '${OC_RUN_OUT}_a.frames.csv' | cmp - '${CMAKE_CURRENT_BINARY_DIR}/accept_hoist_step.frames.csv'")
set_tests_properties(run_frames_hoist_step run_trace_same_with_frames run_frames_same_as_accept_loop
                     PROPERTIES FIXTURES_REQUIRED run_frames)

# open-crank.dbc agrees with the encoders (tools/check_dbc.py, cantools).
# Skipped (77) when cantools is not installed; CI installs it.
add_executable(oc-dbc-vectors dbc_vectors.c)
target_link_libraries(oc-dbc-vectors PRIVATE opencrank_sim m)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME dbc_agrees_with_encoders
             COMMAND sh -c "$<TARGET_FILE:oc-dbc-vectors> | ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_dbc.py ${PROJECT_SOURCE_DIR}/open-crank.dbc")
    set_tests_properties(dbc_agrees_with_encoders PROPERTIES SKIP_RETURN_CODE 77)
endif()
