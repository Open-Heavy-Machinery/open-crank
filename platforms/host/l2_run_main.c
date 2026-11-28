/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file l2_run_main.c
 * @brief open-crank-l2-run: a Level 2 scenario in memory (sim/l2/l2_loop.h):
 *        the Level 2 plant and the simulated machine controller against the
 *        reference engine-ECU logic, through a model of the bridge's edges and
 *        captures. Headless, free-running, deterministic.
 *
 *     open-crank-l2-run scenarios/l2/hold_1500.csv [--trace t.csv] [--criteria c.csv]
 *                       [--verdicts v.csv] [--jitter j.csv] [--quiet]
 *
 * With --criteria (or a <scenario>.accept.csv next to the scenario) the run is
 * graded by open-tick's oracle on the Level 2 observables (l2_accept.h).
 * --jitter writes the jitter budget of the run (docs/acceptance.md).
 * Exit 0 pass (or no criteria), 1 a criterion failed, 2 an error.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opencrank/l2/l2_accept.h>
#include <opencrank/l2/l2_loop.h>
#include <opentick/oracle.h>
#include <opentick/trace.h>

enum { EXIT_PASS = 0, EXIT_FAIL = 1, EXIT_ERROR = 2 };

#define SOI_KEEP 200000U

static OcScenario g_scenario;
static OcL2Loop g_loop;
static OtkOracle g_oracle;
static double g_soi[SOI_KEEP];

static const OtkTraceColumn COLS[] = {
    { "t_s", OTK_COL_REAL, 3 },
    { "start", OTK_COL_INT, 0 },
    { "target_rpm", OTK_COL_REAL, 1 },
    { "hoist_demand_pct", OTK_COL_REAL, 1 },
    { "hoist_load_bar", OTK_COL_REAL, 1 },
    { "angle_deg", OTK_COL_REAL, 3 },
    { "plant_rpm", OTK_COL_REAL, 2 },
    { "fuel_pct", OTK_COL_REAL, 2 },
    { "load_nm", OTK_COL_REAL, 1 },
    { "ecu_sync", OTK_COL_TEXT, 0 },
    { "ecu_rpm", OTK_COL_REAL, 2 },
    { "ecu_state", OTK_COL_TEXT, 0 },
    { "ecu_target_rpm", OTK_COL_REAL, 1 },
    { "ecu_fuel_pct", OTK_COL_REAL, 2 },
    { "ecu_tripped", OTK_COL_INT, 0 },
    { "sync_losses", OTK_COL_UINT, 0 },
    { "injections", OTK_COL_UINT, 0 },
    { "misplaced", OTK_COL_UINT, 0 },
    { "misfires", OTK_COL_UINT, 0 },
    { "soi_err_deg", OTK_COL_REAL, 4 },
    { "width_us", OTK_COL_REAL, 1 },
    { "pump_cmd_pct", OTK_COL_REAL, 1 },
    { "mc_state", OTK_COL_TEXT, 0 },
    { "mc_target_rpm", OTK_COL_REAL, 1 },
};

static const char *sync_name(EcuSync s) {
    static const char *const names[] = { "NONE", "SEEKING", "CRANK", "FULL" };
    return ((unsigned)s < 4U) ? names[s] : "?";
}

static const char *gov_name(EcuGovState s) {
    static const char *const names[] = { "STOPPED", "CRANKING", "RUNNING" };
    return ((unsigned)s < 3U) ? names[s] : "?";
}

static void trace_row(OtkTrace *t, const OcL2Loop *l) {
    const OcL2Plant *p = &l->plant;
    otk_trace_real(t, oc_l2_loop_time_s(l));
    otk_trace_int(t, l->sig.start ? 1 : 0);
    otk_trace_real(t, l->sig.target_rpm);
    otk_trace_real(t, l->sig.hoist_demand_pct);
    otk_trace_real(t, l->sig.hoist_load_bar);
    otk_trace_real(t, oc_l2_acc_deg(p->acc));
    otk_trace_real(t, p->plant.rpm);
    otk_trace_real(t, p->plant.fuel_pct);
    otk_trace_real(t, l->load_torque_nm);
    otk_trace_text(t, sync_name(l->wheel.st.sync));
    otk_trace_real(t, (double)l->wheel.st.rpm);
    otk_trace_text(t, gov_name(l->ecu.state));
    otk_trace_real(t, (double)l->ecu.target_rpm);
    otk_trace_real(t, (double)l->ecu.fuel_pct);
    otk_trace_int(t, l->ecu.overspeed_tripped ? 1 : 0);
    otk_trace_uint(t, l->wheel.st.sync_losses);
    otk_trace_uint(t, p->injections);
    otk_trace_uint(t, p->misplaced);
    otk_trace_uint(t, p->misfires);
    if (l->last.valid) {
        otk_trace_real(t, l->last.soi_err_deg);
        otk_trace_real(t, l->last.width_us);
    } else {
        otk_trace_empty(t);
        otk_trace_empty(t);
    }
    otk_trace_real(t, l->pump_cmd_pct);
    otk_trace_text(t, mc_state_name(l->mc.state));
    otk_trace_real(t, (double)l->mc.target_rpm);
    (void)otk_trace_end_row(t);
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

static void budget_row(FILE *fp, const char *q, const char *declared, double measured, const char *unit,
                       double limit) {
    fprintf(fp, "%s,%s,%.6f,%s,", q, declared, measured, unit);
    if (limit >= 0.0) {
        fprintf(fp, "%s", measured <= limit ? "yes" : "no");
    }
    fprintf(fp, "\n");
}

/* The jitter budget of decision 14: start of injection and width, declared and measured. */
static bool write_jitter(const char *path, const char *scenario, const OcL2Loop *l) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return false;
    }
    const size_t n = l->soi_n;
    for (size_t i = 0; i < n; i++) {
        g_soi[i] = fabs(g_soi[i]);
    }
    qsort(g_soi, n, sizeof(g_soi[0]), cmp_double);
    const double p50 = n ? g_soi[(n - 1U) / 2U] : 0.0;
    const double p99 = n ? g_soi[(size_t)ceil(0.99 * (double)n) - 1U] : 0.0;
    const double max = n ? g_soi[n - 1U] : 0.0;
    fprintf(fp, "# oc-l2-jitter 1; source=open-crank-l2-run; scenario=%s; hsi_bias_ppm=%.0f; delay_steps=%u\n",
            scenario, l->cfg->hsi_bias * 1e6, (unsigned)l->cfg->capture_delay_steps);
    fprintf(fp, "quantity,declared,measured,unit,within\n");
    fprintf(fp, "injections,,%zu,count,\n", n);
    budget_row(fp, "soi_err_p50_deg", "", p50, "deg", -1.0);
    budget_row(fp, "soi_err_p99_deg", "0.1", p99, "deg", 0.1);
    budget_row(fp, "soi_err_max_deg", "0.5", max, "deg", 0.5);
    budget_row(fp, "width_err_max_us", "", l->width_err_max_us, "us", -1.0);
    budget_row(fp, "width_err_max_rel", "", l->width_rel_err_max, "", -1.0);
    budget_row(fp, "width_err_over_0.3pct_us", "1", l->width_measured ? l->width_excess_max_us : 0.0, "us", 1.0);
    return fclose(fp) == 0;
}

static void usage(void) {
    fprintf(stderr, "usage: open-crank-l2-run <scenario.csv> [--trace <out.csv>] [--criteria <file>] "
                    "[--verdicts <out.csv>] [--jitter <out.csv>] [--quiet]\n");
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL;
    const char *trace_path = NULL;
    const char *criteria_path = NULL;
    const char *verdicts_path = NULL;
    const char *jitter_path = NULL;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            criteria_path = argv[++i];
        } else if (strcmp(argv[i], "--verdicts") == 0 && i + 1 < argc) {
            verdicts_path = argv[++i];
        } else if (strcmp(argv[i], "--jitter") == 0 && i + 1 < argc) {
            jitter_path = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
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
    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-l2-run: %s: %s\n", scenario_path, err);
        return EXIT_ERROR;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "open-crank-l2-run: %s has no `end` event\n", scenario_path);
        return EXIT_ERROR;
    }
    char default_criteria[600];
    if (criteria_path == NULL) {
        const size_t n = strlen(scenario_path);
        if (n > 4U && strcmp(scenario_path + n - 4U, ".csv") == 0) {
            (void)snprintf(default_criteria, sizeof(default_criteria), "%.*s.accept.csv", (int)(n - 4U),
                           scenario_path);
            FILE *probe = fopen(default_criteria, "r");
            if (probe != NULL) {
                fclose(probe);
                criteria_path = default_criteria;
            }
        }
    }

    oc_l2_loop_init(&g_loop, &OC_L2_LOOP_D936LA6, &g_scenario);
    oc_l2_loop_keep_errors(&g_loop, g_soi, SOI_KEEP);

    OtkOracleTable table;
    const uint8_t ecu_address = g_loop.cfg->ecu->source_address;
    oc_l2_accept_init(&table, &ecu_address);
    if (criteria_path != NULL &&
        !otk_oracle_load(&g_oracle, criteria_path, &table, &g_scenario, &OC_SIGNAL_TABLE,
                         otk_ns_from_ms(1U), err, sizeof(err))) {
        fprintf(stderr, "open-crank-l2-run: %s: %s\n", criteria_path, err);
        return EXIT_ERROR;
    }

    FILE *trace = NULL;
    OtkTrace tr;
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL || !otk_trace_init(&tr, trace, COLS, sizeof(COLS) / sizeof(COLS[0]))) {
            fprintf(stderr, "open-crank-l2-run: cannot write %s\n", trace_path);
            return EXIT_ERROR;
        }
    }

    if (!quiet) {
        printf("open-crank-l2-run: %s, %s, until %.1f s%s%s\n", scenario_path, g_loop.cfg->plant->name,
               otk_ns_to_s(g_scenario.end), criteria_path ? ", criteria " : "", criteria_path ? criteria_path : "");
    }
    for (;;) {
        if (criteria_path != NULL) {
            OtkCanFrame obs[8];
            const size_t n = oc_l2_observation_frames(&g_loop, obs, 8U);
            for (size_t i = 0; i < n; i++) {
                otk_oracle_frame(&g_oracle, &obs[i], otk_ns_from_ms(g_loop.t_ms));
            }
            otk_oracle_step(&g_oracle, otk_ns_from_ms(g_loop.t_ms));
        }
        if (oc_l2_loop_done(&g_loop)) {
            break;
        }
        OtkCanFrame tx[4];
        const size_t n_tx = oc_l2_loop_step(&g_loop, tx, 4U);
        if (criteria_path != NULL) {
            for (size_t i = 0; i < n_tx; i++) {
                otk_oracle_frame(&g_oracle, &tx[i], otk_ns_from_ms(g_loop.t_ms));
            }
        }
        if (trace != NULL && (g_loop.t_ms % 10U) == 0U) {
            trace_row(&tr, &g_loop);
        }
        if (!quiet && (g_loop.t_ms % 1000U) == 0U) {
            printf("[%6.2fs] rpm %7.1f | ECU %-7s %7.1f rpm %-8s fuel %5.1f %% | inj %u misplaced %u | MC %s\n",
                   oc_l2_loop_time_s(&g_loop), g_loop.plant.plant.rpm, sync_name(g_loop.wheel.st.sync),
                   (double)g_loop.wheel.st.rpm, gov_name(g_loop.ecu.state), (double)g_loop.ecu.fuel_pct,
                   (unsigned)g_loop.plant.injections, (unsigned)g_loop.plant.misplaced, mc_state_name(g_loop.mc.state));
        }
    }

    if (trace != NULL && (fclose(trace) != 0 || !otk_trace_ok(&tr))) {
        fprintf(stderr, "open-crank-l2-run: trace: %s\n", otk_trace_error(&tr));
        return EXIT_ERROR;
    }
    if (g_loop.captures_lost != 0U || g_loop.plant.late_captures != 0U) {
        fprintf(stderr, "open-crank-l2-run: %u captures lost in flight, %u late: the run is broken\n",
                (unsigned)g_loop.captures_lost, (unsigned)g_loop.plant.late_captures);
        return EXIT_ERROR;
    }
    if (jitter_path != NULL && !write_jitter(jitter_path, scenario_path, &g_loop)) {
        fprintf(stderr, "open-crank-l2-run: cannot write %s\n", jitter_path);
        return EXIT_ERROR;
    }
    printf("open-crank-l2-run: %.1f s, %u injections, %u misplaced, %u sync losses, first injection after %.2f rev\n",
           oc_l2_loop_time_s(&g_loop), (unsigned)g_loop.plant.injections, (unsigned)g_loop.plant.misplaced,
           (unsigned)g_loop.wheel.st.sync_losses, g_loop.first_injection_rev);
    if (criteria_path == NULL) {
        return EXIT_PASS;
    }
    const bool pass = otk_oracle_report(&g_oracle, stdout);
    if (verdicts_path != NULL) {
        FILE *vf = fopen(verdicts_path, "w");
        const bool written = vf != NULL && otk_oracle_write_verdicts(&g_oracle, vf);
        if (vf == NULL || fclose(vf) != 0 || !written) {
            fprintf(stderr, "open-crank-l2-run: cannot write %s\n", verdicts_path);
            return EXIT_ERROR;
        }
    }
    const size_t total = otk_oracle_count(&g_oracle);
    if (pass) {
        printf("open-crank-l2-run: PASS (%zu of %zu criteria)\n", total, total);
        return EXIT_PASS;
    }
    printf("open-crank-l2-run: FAIL (%zu of %zu criteria failed)\n", otk_oracle_failures(&g_oracle), total);
    return EXIT_FAIL;
}
