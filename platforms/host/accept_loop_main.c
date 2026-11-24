/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file accept_loop_main.c
 * @brief open-crank-accept-loop: the acceptance suite on the in-memory loop.
 *
 * Grades the reference machine controller (MC_DEFAULT_PARAMS, device under
 * test 0x27), or a mutant built from it, with the same oracle and criteria as
 * open-crank-accept, but in the closed loop of sim/runtime/loop.c: no bus, no
 * wall clock, as fast as the CPU allows. See docs/acceptance.md.
 *
 *     open-crank-accept-loop scenarios/hoist_step.csv [--criteria <file>] [--verdicts <out.csv>]
 *
 * The frames in the loop's to_bench queue are those the controller sent in
 * the previous step; oc_loop_step() hands them to the bench at the start of
 * the step, so the oracle sees them with the same bench time as in the
 * socket runner.
 *
 * Exit codes: 0 pass, 1 fail, 2 usage, format or I/O.
 */

#include <stdio.h>
#include <string.h>

#include <opencrank/accept.h>
#include <opencrank/runtime/loop.h>
#include <opentick/oracle.h>

#define EXIT_PASS 0
#define EXIT_FAIL 1
#define EXIT_ERROR 2

static OcScenario g_scenario;
static OtkOracle g_oracle;
static OcLoop g_loop;

static void usage(void) {
    fprintf(stderr,
            "usage: open-crank-accept-loop <scenario.csv> [--criteria <file>] [--verdicts <out.csv>]\n"
            "  --criteria  defaults to the scenario's path with .csv replaced by .accept.csv\n"
            "  --verdicts  writes the oracle's verdicts, one row per criterion (open-tick docs/verdict-format.md)\n"
            "  Grades the reference machine controller (0x%02X) in the in-memory loop.\n",
            (unsigned)OC_ACCEPT_DUT_DEFAULT);
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL;
    const char *criteria_path = NULL;
    const char *verdicts_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            criteria_path = argv[++i];
        } else if (strcmp(argv[i], "--verdicts") == 0 && i + 1 < argc) {
            verdicts_path = argv[++i];
        } else if (argv[i][0] != '-' && scenario_path == NULL) {
            scenario_path = argv[i];
        } else {
            usage();
            return EXIT_ERROR;
        }
    }
    if (scenario_path == NULL) {
        usage();
        return EXIT_ERROR;
    }

    char default_criteria[4096];
    if (criteria_path == NULL) {
        const size_t n = strlen(scenario_path);
        if (n < 4U || strcmp(&scenario_path[n - 4U], ".csv") != 0 || n + 8U > sizeof(default_criteria)) {
            fprintf(stderr, "open-crank-accept-loop: the scenario's name does not end in .csv; give --criteria\n");
            return EXIT_ERROR;
        }
        (void)snprintf(default_criteria, sizeof(default_criteria), "%.*s.accept.csv", (int)(n - 4U), scenario_path);
        criteria_path = default_criteria;
    }

    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-accept-loop: %s: %s\n", scenario_path, err);
        return EXIT_ERROR;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "open-crank-accept-loop: %s has no end event; the run would never finish\n", scenario_path);
        return EXIT_ERROR;
    }

    oc_loop_init(&g_loop, &OC_BENCH_D936LA6, &MC_DEFAULT_PARAMS, &g_scenario);

    OcAccept accept;
    OtkOracleTable table;
    oc_accept_init(&accept, MC_DEFAULT_PARAMS.source_address, &table);
    if (!otk_oracle_load(&g_oracle, criteria_path, &table, &g_scenario, &OC_SIGNAL_TABLE,
                         otk_ns_from_ms(g_loop.bench.cfg->step_ms), err, sizeof(err))) {
        fprintf(stderr, "open-crank-accept-loop: %s: %s\n", criteria_path, err);
        return EXIT_ERROR;
    }

    printf("open-crank-accept-loop: [%s] in memory, scenario %s, criteria %s, reference controller 0x%02X\n",
           g_loop.bench.cfg->plant->name, scenario_path, criteria_path, (unsigned)MC_DEFAULT_PARAMS.source_address);

    for (;;) {
        for (size_t i = 0; i < otk_can_queue_count(&g_loop.to_bench); i++) {
            otk_oracle_frame(&g_oracle, otk_can_queue_at(&g_loop.to_bench, i), otk_ns_from_ms(g_loop.bench.t_ms));
        }
        otk_oracle_step(&g_oracle, otk_ns_from_ms(g_loop.bench.t_ms));
        if (oc_bench_scenario_done(&g_loop.bench)) {
            break;
        }
        oc_loop_step(&g_loop);
    }

    const bool pass = otk_oracle_report(&g_oracle, stdout);
    if (verdicts_path != NULL) {
        FILE *vf = fopen(verdicts_path, "w");
        const bool written = vf != NULL && otk_oracle_write_verdicts(&g_oracle, vf);
        if (vf == NULL || fclose(vf) != 0 || !written) {
            fprintf(stderr, "open-crank-accept-loop: cannot write %s\n", verdicts_path);
            return EXIT_ERROR;
        }
    }
    const size_t total = otk_oracle_count(&g_oracle);
    if (pass) {
        printf("open-crank-accept-loop: PASS (%zu of %zu criteria)\n", total, total);
        return EXIT_PASS;
    }
    printf("open-crank-accept-loop: FAIL (%zu of %zu criteria failed)\n", otk_oracle_failures(&g_oracle), total);
    return EXIT_FAIL;
}
