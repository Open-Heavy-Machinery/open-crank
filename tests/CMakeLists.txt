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
