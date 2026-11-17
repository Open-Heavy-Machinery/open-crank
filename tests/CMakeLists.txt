# Unit and closed-loop tests. All headless and free-running: no sockets, no
# wall clock. They use CHECK (tests/check.h), never assert(), so Release
# builds still test.
set(OC_TESTS
    test_j1939
    test_machine_ctrl
    test_plant
    test_hydraulic_pump
    test_engine_ecu
    test_bench
    test_scenario
    test_closed_loop
)
foreach(test_name ${OC_TESTS})
    add_executable(${test_name} ${test_name}.c)
    target_link_libraries(${test_name} PRIVATE opencrank_sim m)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

set(OC_SCENARIO_DIR ${PROJECT_SOURCE_DIR}/scenarios)
target_compile_definitions(test_closed_loop PRIVATE OC_SCENARIO_DIR="${OC_SCENARIO_DIR}")

# Every shipped scenario must parse, so a broken demo file fails the build.
file(GLOB OC_SCENARIO_FILES CONFIGURE_DEPENDS ${OC_SCENARIO_DIR}/*.csv)
if(NOT OC_SCENARIO_FILES)
    message(FATAL_ERROR "no scenario files found in ${OC_SCENARIO_DIR}")
endif()
add_test(NAME test_scenario_files COMMAND test_scenario ${OC_SCENARIO_FILES})

# Smoke test of the headless runner, as a user would call it.
add_test(NAME run_hoist_step COMMAND open-crank-run ${OC_SCENARIO_DIR}/hoist_step.csv --quiet)
