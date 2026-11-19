/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file run_main.c
 * @brief open-crank-run: run a scenario closed loop, headless and free-running.
 *
 * Bench and reference machine controller in one process over an in-memory
 * bus: no CAN interface, no root, no wall clock. A 25 s scenario takes a few
 * milliseconds. Same code path as the closed-loop tests.
 *
 *     open-crank-run scenarios/hoist_step.csv --trace hoist.csv
 */

#include <stdio.h>
#include <string.h>

#include <opencrank/runtime/loop.h>
#include <opencrank/runtime/trace.h>

static OcScenario g_scenario;
static OcLoop g_loop;

static void usage(void) {
    fprintf(stderr,
            "usage: open-crank-run <scenario.csv> [--trace <out.csv>] [--quiet]\n"
            "  Runs the scenario with the reference machine controller until its `end` event.\n");
}

static void print_status(const OcLoop *l) {
    const OcBench *b = &l->bench;
    printf("[%6.2fs] rpm %7.1f | fuel %5.1f %% | load %6.1f Nm | ECU %-8s%s | MC %-11s %s\n",
           oc_bench_time_s(b), b->plant.rpm, b->plant.fuel_pct, b->load_torque_nm, oc_ecu_state_name(b->ecu.state),
           b->ecu.overspeed_tripped ? " TRIPPED" : "", mc_state_name(l->mc.state),
           (l->mc.state == MC_STATE_FAULT) ? mc_fault_name(l->mc.fault) : "");
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL;
    const char *trace_path = NULL;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (argv[i][0] != '-' && scenario_path == NULL) {
            scenario_path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (scenario_path == NULL) {
        usage();
        return 2;
    }

    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-run: %s\n", err);
        return 1;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "open-crank-run: %s has no `end` event; add one such as '30.0,end,0'\n", scenario_path);
        return 1;
    }

    FILE *trace = NULL;
    OtkTrace tr;
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL) {
            perror("open-crank-run: cannot open trace file");
            return 1;
        }
        if (!oc_trace_open(&tr, trace)) {
            fprintf(stderr, "open-crank-run: trace: %s\n", otk_trace_error(&tr));
            return 1;
        }
    }

    oc_loop_init(&g_loop, &OC_BENCH_D936LA6, &MC_DEFAULT_PARAMS, &g_scenario);
    if (!quiet) {
        printf("open-crank-run: %s, %s, until %.1f s\n", scenario_path, g_loop.bench.cfg->plant->name,
               otk_ns_to_s(g_scenario.end));
    }
    while (!oc_bench_scenario_done(&g_loop.bench)) {
        oc_loop_step(&g_loop);
        if (trace != NULL) {
            oc_trace_row(&tr, &g_loop.bench, &g_loop.mc);
        }
        if (!quiet && g_loop.bench.t_ms % 1000U == 0U) {
            print_status(&g_loop);
        }
    }
    if (quiet) {
        print_status(&g_loop);
    }

    if (trace != NULL && !otk_trace_ok(&tr)) {
        fprintf(stderr, "open-crank-run: trace: %s\n", otk_trace_error(&tr));
    }
    if (trace != NULL && (fclose(trace) != 0 || !otk_trace_ok(&tr))) {
        perror("open-crank-run: writing trace");
        return 1;
    }
    return 0;
}
