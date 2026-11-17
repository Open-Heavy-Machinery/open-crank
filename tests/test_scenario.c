/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_scenario.c
 * @brief Scenario CSV parser and event timing.
 */

#include <string.h>

#include "check.h"
#include <opencrank/runtime/bench.h>
#include <opencrank/runtime/scenario.h>

static OcScenario scn; /* large: keep off the stack */

static void test_parses_valid_text(void) {
    const char *text =
        "# a comment\n"
        "\n"
        "time_s,signal,value\r\n"
        "0.0,key_on,1\n"
        "  0.5 , start , 1  \n"
        "3,start,0\n"
        "8.25,hoist_demand_pct,85.5\n"
        "8.25,fault.overrun_torque_nm,-12.5\n"
        "20,end,0\n";
    char err[128] = "";
    CHECK(oc_scenario_parse(text, &scn, err, sizeof(err)));
    CHECK(scn.n == 6U);
    CHECK(scn.events[1].signal == OC_SIG_START && scn.events[1].t_ms == 500U && scn.events[1].value == 1.0);
    CHECK(scn.events[3].t_ms == 8250U && scn.events[3].value == 85.5);
    CHECK(scn.events[4].signal == OC_SIG_FAULT_OVERRUN_TORQUE_NM);
    CHECK(scn.has_end && scn.end_ms == 20000U);
}

static void expect_error(const char *text, const char *fragment) {
    char err[160] = "";
    CHECK(!oc_scenario_parse(text, &scn, err, sizeof(err)));
    CHECK(strstr(err, fragment) != NULL);
    if (strstr(err, fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", err, fragment);
    }
}

static void test_rejects_bad_text(void) {
    expect_error("time_s,signal,value\n0,key_on,1\n1,kye_on,1\n", "line 3: unknown signal 'kye_on'");
    expect_error("time_s,signal,value\n2,key_on,1\n1,start,1\n", "line 3: time 1.000 s is earlier");
    expect_error("0,key_on,1\n", "line 1: expected the header");
    expect_error("time_s,signal,value\n0,key_on,abc\n", "line 2: bad value 'abc'");
    expect_error("time_s,signal,value\n-1,key_on,1\n", "line 2: bad time '-1'");
    expect_error("time_s,signal,value\n0,key_on\n", "line 2: expected three fields");
    expect_error("time_s,signal,value\n0,key_on,1,2\n", "line 2: expected three fields");
    expect_error("time_s,signal,value\n0,end,0\n1,key_on,1\n", "line 3: event after 'end'");
    expect_error("time_s,signal,value\n0,key_on,nan\n", "line 2: bad value 'nan'");
    expect_error("# only a comment\n", "empty scenario");
}

static void test_events_apply_in_their_step(void) {
    const char *text =
        "time_s,signal,value\n"
        "0.015,target_rpm,1000\n"
        "0.015,target_rpm,1200\n";
    char err[128] = "";
    CHECK(oc_scenario_parse(text, &scn, err, sizeof(err)));

    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    oc_bench_set_scenario(&b, &scn);
    J1939Frame tx[8];
    (void)oc_bench_step(&b, tx, 8); /* step starting at 0 ms */
    (void)oc_bench_step(&b, tx, 8); /* step starting at 10 ms */
    CHECK(b.sig.target_rpm == 0.0);
    (void)oc_bench_step(&b, tx, 8); /* step starting at 20 ms: the 15 ms events are due */
    CHECK(b.sig.target_rpm == 1200.0); /* same time: file order */
    CHECK(!oc_bench_scenario_done(&b));
}

int main(void) {
    RUN_TEST(test_parses_valid_text);
    RUN_TEST(test_rejects_bad_text);
    RUN_TEST(test_events_apply_in_their_step);
    return check_report();
}
