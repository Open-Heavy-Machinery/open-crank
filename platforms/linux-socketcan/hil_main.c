/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hil_main.c
 * @brief open-crank-hil: a Level 2 scenario against a real engine ECU (v0.4).
 *
 *   open-crank-hil <scenario.csv> --dev <bridge UART> --can <iface>
 *                  [--criteria c.csv] [--trace t.csv] [--frames f.csv]
 *                  [--verdicts v.csv] [--jitter j.csv] [--budget b.csv]
 *                  [--fidelity <machine.csv> --bridge-fidelity <bridge.csv>]
 *                  [--bitstream-md5 M] [--commit C] [--quiet]
 *
 * The machine side of the Level 2 loop (sim/l2/l2_loop.h with an external
 * ECU: the Level 2 plant, the hoist pump, the reference machine controller
 * and the operator panel) runs on the host, one step per 1 ms, through
 * open-tick's hard loop (hard_loop.h, H = 16): each step's crank speed goes
 * to the bridge as its crank_phase_inc, so the bridge's crank and cam are the
 * plant's angle; the bridge captures the ECU's injector pulses, and each
 * capture fuels the plant's cylinders by its own tick. The controller's TSC1
 * goes out on the CAN interface at its step, on the bridge's clock; the
 * ECU's EEC1 comes back from it to the controller.
 *
 * The criteria (a <scenario>.accept.csv next to the scenario, or
 * --criteria) are judged by open-tick's oracle on the same Level 2
 * observables as the in-memory loop (l2_accept.h).
 *
 * --fidelity checks, before the bridge is opened, that the machine's
 * fidelity file (sim/l2/fidelity.csv) is within what the bridge guarantees,
 * with this run's lead and DUT-to-plant delay (open-tick's
 * otk_hard_dut_to_plant_ns(): (H + 1) steps, two forwarding cycles and a round
 * trip, 31.2 ms).
 *
 * Exit 0 pass (or no criteria), 1 a criterion failed, 2 an error, 3 the hard
 * run was not ok (degraded or aborted), 5 the fidelity check refused the run.
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opencrank/l2/l2_accept.h>
#include <opencrank/l2/l2_loop.h>
#include <opentick/fidelity.h>
#include <opentick/framelog.h>
#include <opentick/host/hard_loop.h>
#include <opentick/host/socketcan.h>
#include <opentick/oracle.h>
#include <opentick/trace.h>

enum { EXIT_PASS = 0, EXIT_FAIL = 1, EXIT_ERROR = 2, EXIT_HARD = 3, EXIT_FIDELITY = 5 };

#define SOI_KEEP 400000U
#define STEP_MS 1U
#define CYCLE_NS 6000000U /* the BL616's forwarding cycle, as open-tick's hard loop guesses it */
/* The plant judges a cylinder's window only this many steps after it: beyond
 * the DUT-to-plant delay a capture can have on the BL616 link (open-tick
 * otk_hard_dut_to_plant_ns(): 31.2 ms with H = 16; 29-30 measured), with room. */
#define HIL_CAPTURE_DELAY_STEPS 40U

static OcScenario g_scenario;
static OcL2Loop g_loop;
static OcL2LoopConfig g_cfg;
static OcL2Params g_l2;
static OtkOracle g_oracle;
static double g_soi[SOI_KEEP];
static OtkHardLoop g_hard;
static OtkSocketCan g_can;
static OtkFidelityFile g_fid_machine, g_fid_bridge;

/* ---- the trace ---------------------------------------------------------------- */

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
    { "ecu_rpm", OTK_COL_REAL, 2 },
    { "injections", OTK_COL_UINT, 0 },
    { "misplaced", OTK_COL_UINT, 0 },
    { "misfires", OTK_COL_UINT, 0 },
    { "soi_err_deg", OTK_COL_REAL, 4 },
    { "width_us", OTK_COL_REAL, 1 },
    { "pump_cmd_pct", OTK_COL_REAL, 1 },
    { "mc_state", OTK_COL_TEXT, 0 },
    { "mc_target_rpm", OTK_COL_REAL, 1 },
    { "capture_lag_max", OTK_COL_UINT, 0 },
};

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
    if (l->ecu_eec1_seen) {
        otk_trace_real(t, l->ecu_eec1_rpm);
    } else {
        otk_trace_empty(t);
    }
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
    otk_trace_uint(t, l->capture_lag_max);
    (void)otk_trace_end_row(t);
}

/* ---- the frame log and the CAN send error (as otk-flywheel's hard run) -------- */

typedef struct {
    OtkNs t, t_bus;
    OtkFrameDir dir;
    OtkCanFrame frame;
} Row;
static Row *g_rows;
static size_t g_nrows, g_caprows;
static uint64_t *g_tx_err;
static size_t g_ntx, g_captx;
static uint32_t g_sent_step[1024];
static size_t g_sent_head, g_sent_n;
static bool g_oom;

static void *grow(void *p, size_t *cap, size_t n, size_t size) {
    if (n < *cap) {
        return p;
    }
    const size_t c = *cap ? 2U * *cap : 4096U;
    void *q = realloc(p, c * size);
    if (q == NULL) {
        g_oom = true;
        return p;
    }
    *cap = c;
    return q;
}

static void on_can_sent(void *ctx, const OtkHardCanSent *sent) {
    (void)ctx;
    if (sent->ok && g_sent_n < sizeof(g_sent_step) / sizeof(g_sent_step[0])) {
        g_sent_step[(g_sent_head + g_sent_n) % 1024U] = sent->step;
        g_sent_n++;
    }
}

static void add_row(const Row *r) {
    g_rows = grow(g_rows, &g_caprows, g_nrows, sizeof(Row));
    if (g_nrows < g_caprows) {
        g_rows[g_nrows++] = *r;
    }
}

/* Frames from the bus: the ECU's to the controller and the oracle; our own copies for the log. */
static void read_bus(bool judge) {
    OtkCanFrame f;
    OtkSocketCanRxInfo info;
    while (otk_socketcan_recv_ex(&g_can, &f, &info) == 1) {
        int64_t bus = 0;
        const bool mapped = info.has_time && otk_hard_loop_bridge_ns_rt(&g_hard, info.time_ns, &bus) && bus >= 0;
        Row r;
        r.frame = f;
        r.t_bus = mapped ? (OtkNs)bus : OTK_FRAMELOG_NO_TIME;
        if (info.own) {
            if (g_sent_n == 0U) {
                continue;
            }
            const uint32_t step = g_sent_step[g_sent_head];
            g_sent_head = (g_sent_head + 1U) % 1024U;
            g_sent_n--;
            r.t = otk_ns_from_ms((uint64_t)step * STEP_MS);
            r.dir = OTK_FRAME_TO_DUT;
            if (mapped) {
                g_tx_err = grow(g_tx_err, &g_captx, g_ntx, sizeof(uint64_t));
                if (g_ntx < g_captx) {
                    g_tx_err[g_ntx++] = r.t_bus > r.t ? r.t_bus - r.t : r.t - r.t_bus;
                }
            }
        } else {
            r.t = otk_ns_from_ms(g_loop.t_ms);
            r.dir = OTK_FRAME_FROM_DUT;
            oc_l2_loop_rx(&g_loop, &f);
            if (judge) {
                otk_oracle_frame(&g_oracle, &f, otk_ns_from_ms(g_loop.t_ms));
            }
        }
        add_row(&r);
    }
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static OtkHardCanErr can_err(void) {
    OtkHardCanErr e = { 0U, 0U, 0U, 0U };
    if (g_ntx == 0U) {
        return e;
    }
    qsort(g_tx_err, g_ntx, sizeof(uint64_t), cmp_u64);
    e.n = g_ntx;
    e.p50_ns = g_tx_err[(size_t)ceil(0.50 * (double)g_ntx) - 1U];
    e.p99_ns = g_tx_err[(size_t)ceil(0.99 * (double)g_ntx) - 1U];
    e.max_ns = g_tx_err[g_ntx - 1U];
    return e;
}

static bool write_frames(const char *path) {
    const uint64_t err_ns = otk_hard_loop_bus_err_ns(&g_hard);
    const OtkFrameLogConfig cfg = { otk_ns_from_ms(STEP_MS), OTK_BUS_CLOCK_BRIDGE, err_ns != UINT64_MAX,
                                    err_ns == UINT64_MAX ? 0U : err_ns, "open-crank-hil" };
    FILE *fp = fopen(path, "w");
    OtkFrameLog log;
    bool ok = fp != NULL && otk_framelog_init(&log, fp, &cfg);
    for (size_t i = 0; ok && i < g_nrows; i++) {
        const OtkFrameRow row = { g_rows[i].t, g_rows[i].t_bus, g_rows[i].dir, NULL, &g_rows[i].frame, OTK_FRAME_OK };
        ok = otk_framelog_frame(&log, &row);
    }
    if (fp != NULL && !ok) {
        fprintf(stderr, "open-crank-hil: frame log: %s\n", otk_framelog_error(&log));
    }
    if (fp == NULL || fclose(fp) != 0) {
        ok = false;
    }
    return ok;
}

/* ---- the jitter budget (decision 14), measured from the captures -------------- */

static bool write_jitter(const char *path, const char *scenario, const char *md5, const char *commit) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return false;
    }
    const size_t n = g_loop.soi_n;
    for (size_t i = 0; i < n; i++) {
        g_soi[i] = fabs(g_soi[i]);
    }
    qsort(g_soi, n, sizeof(g_soi[0]), cmp_double);
    const double p50 = n ? g_soi[(n - 1U) / 2U] : 0.0;
    const double p99 = n ? g_soi[(size_t)ceil(0.99 * (double)n) - 1U] : 0.0;
    const double max = n ? g_soi[n - 1U] : 0.0;
    fprintf(fp, "# oc-l2-jitter 1; source=open-crank-hil; scenario=%s; capture_lag_max_steps=%u; "
                "bitstream_md5=%s; commit=%s\n",
            scenario, (unsigned)g_loop.capture_lag_max, md5, commit);
    fprintf(fp, "quantity,declared,measured,unit,within\n");
    fprintf(fp, "injections,,%zu,count,\n", n);
    fprintf(fp, "soi_err_p50_deg,,%.6f,deg,\n", p50);
    fprintf(fp, "soi_err_p99_deg,0.1,%.6f,deg,%s\n", p99, p99 <= 0.1 ? "yes" : "no");
    fprintf(fp, "soi_err_max_deg,0.5,%.6f,deg,%s\n", max, max <= 0.5 ? "yes" : "no");
    /* The host does not know the width the ECU commanded: the widths are
     * measured against a known width at constant speed (open-crank-ecu-check). */
    fprintf(fp, "width_err_max_us,,,us,\n");
    fprintf(fp, "width_err_max_rel,,,,\n");
    fprintf(fp, "width_err_over_0.3pct_us,1,,us,\n");
    return fclose(fp) == 0;
}

/* ---- fidelity (as otk-flywheel's --fidelity) ----------------------------------- */

static bool read_fidelity(const char *path, OtkFidelityFile *f) {
    static char text[65536];
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "open-crank-hil: cannot read %s: %s\n", path, strerror(errno));
        return false;
    }
    const size_t n = fread(text, 1U, sizeof(text), fp);
    const bool too_long = n == sizeof(text);
    fclose(fp);
    if (too_long) {
        fprintf(stderr, "open-crank-hil: %s is too long\n", path);
        return false;
    }
    OtkFidelityError e;
    if (!otk_fidelity_parse(text, n, f, &e)) {
        fprintf(stderr, "open-crank-hil: %s, line %u%s%s: %s\n", path, e.line, e.column ? ", column " : "",
                e.column ? e.column : "", e.what);
        return false;
    }
    return true;
}

static int check_fidelity(const char *machine_path, const char *bridge_path, unsigned h, uint64_t rtt_ns) {
    if (!read_fidelity(machine_path, &g_fid_machine) || !read_fidelity(bridge_path, &g_fid_bridge)) {
        return EXIT_ERROR;
    }
    /* the lead and the delay are this run's (open-tick §7): H steps; the delay a capture can have (§2) */
    g_fid_bridge.has_lead = g_fid_bridge.has_delay = true;
    g_fid_bridge.lead_ns = (uint64_t)h * STEP_MS * 1000000U;
    g_fid_bridge.dut_to_plant_delay_ns = otk_hard_dut_to_plant_ns(h, STEP_MS * 1000000U, CYCLE_NS, rtt_ns);
    OtkFidelityResult r;
    char msg[512];
    const OtkFidelityVerdict v = otk_fidelity_check(&g_fid_machine, &g_fid_bridge, &r);
    otk_fidelity_describe(&r, &g_fid_machine, &g_fid_bridge, msg, sizeof(msg));
    fprintf(v == OTK_FIDELITY_OK ? stdout : stderr,
            "open-crank-hil: %s (lead %llu ns, DUT-to-plant delay %llu ns: H = %u steps of %u ms)\n", msg,
            (unsigned long long)g_fid_bridge.lead_ns, (unsigned long long)g_fid_bridge.dut_to_plant_delay_ns, h,
            STEP_MS);
    return v == OTK_FIDELITY_OK ? EXIT_PASS : EXIT_FIDELITY;
}

/* What a hard run of open-crank-hil is declared to hold on the BL616 link: otk-flywheel's figures
 * (open-tick docs/bridge-protocol.md §9, at the 1.2 ms margin). */
static OtkHardBudget declared_budget(void) {
    OtkHardBudget b;
    otk_hard_budget_none(&b);
    b.underruns = 0U;
    b.late = 0U;
    b.rtt_p99_ns = 2200000U;
    b.mapping_bound_ns = 1000000U;
    b.realtime_err_ns = 10000U;
    b.can_tx_err_ns = 1100000U;
    return b;
}

static void usage(void) {
    fprintf(stderr, "usage: open-crank-hil <scenario.csv> --dev <bridge UART> --can <iface> [--criteria <file>] "
                    "[--trace <out.csv>] [--frames <out.csv>] [--verdicts <out.csv>] [--jitter <out.csv>] "
                    "[--budget <out.csv>] [--fidelity <machine.csv> --bridge-fidelity <bridge.csv>] "
                    "[--bitstream-md5 M] [--commit C] [--quiet]\n");
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL, *dev = NULL, *can_if = NULL, *criteria_path = NULL;
    const char *trace_path = NULL, *frames_path = NULL, *verdicts_path = NULL, *jitter_path = NULL;
    const char *budget_path = NULL, *fid_path = NULL, *bridge_fid_path = NULL;
    const char *md5 = "", *commit = "";
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = NULL;
        if (!strcmp(a, "--dev")) { slot = &dev; }
        else if (!strcmp(a, "--can")) { slot = &can_if; }
        else if (!strcmp(a, "--criteria")) { slot = &criteria_path; }
        else if (!strcmp(a, "--trace")) { slot = &trace_path; }
        else if (!strcmp(a, "--frames")) { slot = &frames_path; }
        else if (!strcmp(a, "--verdicts")) { slot = &verdicts_path; }
        else if (!strcmp(a, "--jitter")) { slot = &jitter_path; }
        else if (!strcmp(a, "--budget")) { slot = &budget_path; }
        else if (!strcmp(a, "--fidelity")) { slot = &fid_path; }
        else if (!strcmp(a, "--bridge-fidelity")) { slot = &bridge_fid_path; }
        else if (!strcmp(a, "--bitstream-md5")) { slot = &md5; }
        else if (!strcmp(a, "--commit")) { slot = &commit; }
        else if (!strcmp(a, "--quiet")) { quiet = true; continue; }
        else if (a[0] != '-' && scenario_path == NULL) { scenario_path = a; continue; }
        if (slot == NULL || i + 1 >= argc) {
            usage();
            return EXIT_ERROR;
        }
        *slot = argv[++i];
    }
    if (scenario_path == NULL || dev == NULL || can_if == NULL || ((fid_path == NULL) != (bridge_fid_path == NULL))) {
        usage();
        return EXIT_ERROR;
    }

    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-hil: %s: %s\n", scenario_path, err);
        return EXIT_ERROR;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "open-crank-hil: %s has no `end` event\n", scenario_path);
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

    OtkHardLoopConfig hc;
    otk_hard_loop_defaults(&hc, OC_L2_STEP_TICKS);
    oc_l2_bridge_config(&hc.bridge);
    hc.need_tick_hz = OC_L2_TICK_HZ;
    hc.need_capture = (uint8_t)OC_L2_CYLINDERS;
    if (fid_path != NULL) {
        /* before the bridge is opened: a round trip as declared on this link */
        const int rc = check_fidelity(fid_path, bridge_fid_path, hc.h, declared_budget().rtt_p99_ns);
        if (rc != EXIT_PASS) {
            return rc;
        }
    }

    g_cfg = OC_L2_LOOP_D936LA6;
    g_cfg.capture_delay_steps = HIL_CAPTURE_DELAY_STEPS;
    g_l2 = *g_cfg.l2; /* the plant judges a window only once its injection can have arrived */
    g_l2.capture_delay_steps = HIL_CAPTURE_DELAY_STEPS;
    g_cfg.l2 = &g_l2;
    oc_l2_loop_init_external(&g_loop, &g_cfg, &g_scenario);
    oc_l2_loop_keep_errors(&g_loop, g_soi, SOI_KEEP);

    OtkOracleTable table;
    const uint8_t ecu_address = g_cfg.ecu->source_address;
    oc_l2_accept_init(&table, &ecu_address);
    if (criteria_path != NULL &&
        !otk_oracle_load(&g_oracle, criteria_path, &table, &g_scenario, &OC_SIGNAL_TABLE, otk_ns_from_ms(1U), err,
                         sizeof(err))) {
        fprintf(stderr, "open-crank-hil: %s: %s\n", criteria_path, err);
        return EXIT_ERROR;
    }
    FILE *trace = NULL;
    OtkTrace tr;
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL || !otk_trace_init(&tr, trace, COLS, sizeof(COLS) / sizeof(COLS[0]))) {
            fprintf(stderr, "open-crank-hil: cannot write %s\n", trace_path);
            return EXIT_ERROR;
        }
    }

    if (!otk_socketcan_open(&g_can, can_if) || !otk_socketcan_enable_timestamps(&g_can, true)) {
        fprintf(stderr, "open-crank-hil: cannot open %s with timestamps: %s\n", can_if, strerror(errno));
        return EXIT_ERROR;
    }
    if (otk_hard_loop_open(&g_hard, dev, &hc, err, sizeof(err)) != 0) {
        fprintf(stderr, "open-crank-hil: %s: %s\n", dev, err);
        otk_socketcan_close(&g_can);
        return EXIT_HARD;
    }
    otk_hard_loop_attach_can(&g_hard, &g_can, on_can_sent, NULL);
    if (!quiet) {
        printf("open-crank-hil: %s, %s, until %.1f s, H = %u%s%s\n", scenario_path, g_cfg.plant->name,
               otk_ns_to_s(g_scenario.end), hc.h, criteria_path ? ", criteria " : "",
               criteria_path ? criteria_path : "");
    }

    bool started = false;
    uint64_t t_start = 0U;
    for (;;) {
        /* what came back: the ECU's frames and the bridge's captures */
        read_bus(criteria_path != NULL);
        OtkHardCapture caps[64];
        size_t nc;
        while (started && (nc = otk_hard_loop_captures(&g_hard, caps, 64U)) != 0U) {
            for (size_t i = 0; i < nc; i++) {
                if (caps[i].start_tick >= t_start && caps[i].channel < OC_L2_CYLINDERS) {
                    (void)oc_l2_loop_capture(&g_loop, caps[i].channel, caps[i].start_tick - t_start, caps[i].width);
                }
            }
        }
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
        const uint32_t k = g_loop.t_ms; /* the step computed now: the bridge's step k */
        OtkCanFrame tx[4];
        const size_t n_tx = oc_l2_loop_step(&g_loop, tx, 4U);
        for (size_t i = 0; i < n_tx; i++) {
            (void)otk_hard_loop_can_at(&g_hard, &tx[i], k);
        }
        OtkBridgeSetpoint sp;
        memset(&sp, 0, sizeof(sp));
        sp.crank_phase_inc = g_loop.plant.inc;
        otk_hard_loop_set(&g_hard, &sp);
        if (trace != NULL && (g_loop.t_ms % 10U) == 0U) {
            trace_row(&tr, &g_loop);
        }
        if (!quiet && (g_loop.t_ms % 1000U) == 0U) {
            printf("[%6.2fs] rpm %7.1f | ECU (EEC1) %7.1f rpm | fuel %5.1f %% | inj %u misplaced %u misfires %u | "
                   "MC %s | capture lag max %u steps | underruns %u late %u, exchanges %llu\n",
                   oc_l2_loop_time_s(&g_loop), g_loop.plant.plant.rpm, g_loop.ecu_eec1_rpm, g_loop.plant.plant.fuel_pct,
                   (unsigned)g_loop.plant.injections, (unsigned)g_loop.plant.misplaced,
                   (unsigned)g_loop.plant.misfires, mc_state_name(g_loop.mc.state),
                   (unsigned)g_loop.capture_lag_max, g_hard.stats.underruns, g_hard.stats.late,
                   (unsigned long long)g_hard.stats.exchanges);
            fflush(stdout);
        }
        if (!otk_hard_loop_next(&g_hard)) {
            break;
        }
        if (!started && g_hard.started) {
            started = true;
            t_start = g_hard.aim.t_start;
        }
    }
    const OtkHardResult result = otk_hard_loop_stop(&g_hard);
    otk_hard_sleep_until(otk_hard_now_ns() + 50000000U); /* the last frames' own copies */
    read_bus(false);
    otk_socketcan_close(&g_can);

    int rc = EXIT_PASS;
    if (trace != NULL && (fclose(trace) != 0 || !otk_trace_ok(&tr))) {
        fprintf(stderr, "open-crank-hil: trace: %s\n", otk_trace_error(&tr));
        rc = EXIT_ERROR;
    }
    if (frames_path != NULL && !write_frames(frames_path)) {
        rc = EXIT_ERROR;
    }
    if (jitter_path != NULL && !write_jitter(jitter_path, scenario_path, md5, commit)) {
        fprintf(stderr, "open-crank-hil: cannot write %s\n", jitter_path);
        rc = EXIT_ERROR;
    }
    if (budget_path != NULL) {
        char usb[256];
        (void)otk_hard_usb_path(dev, usb, sizeof(usb));
        const OtkHardBudget b = declared_budget();
        const OtkHardRunMeta meta = { "bl616-3mbaud", usb, md5, commit, "open-crank-hil" };
        const OtkHardCanErr ce = can_err();
        FILE *bf = fopen(budget_path, "w");
        const bool w = bf != NULL && otk_hard_budget_write(bf, &g_hard, result, &b, &meta, &ce);
        if (bf == NULL || fclose(bf) != 0 || !w) {
            fprintf(stderr, "open-crank-hil: cannot write %s\n", budget_path);
            rc = EXIT_ERROR;
        }
    }
    const OtkHardStats *st = &g_hard.stats;
    printf("open-crank-hil: hard run %s: %u bridge steps, %u underruns, %u late, %u timeouts, %u captures lost; "
           "round trip p99 %.0f us\n",
           otk_hard_result_name(result), st->steps, st->underruns, st->late, st->timeouts,
           st->capture_lost + g_hard.cap_dropped, (double)otk_hard_loop_rtt_pct(&g_hard, 99.0) / 1e3);
    printf("open-crank-hil: %.1f s, %u injections, %u misplaced, %u misfires, first injection after %.2f rev, "
           "capture lag max %u steps\n",
           oc_l2_loop_time_s(&g_loop), (unsigned)g_loop.plant.injections, (unsigned)g_loop.plant.misplaced,
           (unsigned)g_loop.plant.misfires, g_loop.first_injection_rev, (unsigned)g_loop.capture_lag_max);
    if (g_oom) {
        fprintf(stderr, "open-crank-hil: out of memory: the frame log or the CAN figures are incomplete\n");
        rc = EXIT_ERROR;
    }
    if (g_loop.captures_lost != 0U || g_loop.plant.late_captures != 0U) {
        fprintf(stderr, "open-crank-hil: %u captures lost in flight, %u too late for the plant's history\n",
                (unsigned)g_loop.captures_lost, (unsigned)g_loop.plant.late_captures);
        rc = EXIT_ERROR;
    }
    bool pass = true;
    if (criteria_path != NULL) {
        pass = otk_oracle_report(&g_oracle, stdout);
        if (verdicts_path != NULL) {
            FILE *vf = fopen(verdicts_path, "w");
            const bool written = vf != NULL && otk_oracle_write_verdicts(&g_oracle, vf);
            if (vf == NULL || fclose(vf) != 0 || !written) {
                fprintf(stderr, "open-crank-hil: cannot write %s\n", verdicts_path);
                rc = EXIT_ERROR;
            }
        }
    }
    otk_hard_loop_free(&g_hard);
    free(g_rows);
    free(g_tx_err);
    if (rc != EXIT_PASS) {
        return rc;
    }
    if (result != OTK_HARD_OK) {
        return EXIT_HARD;
    }
    return pass ? EXIT_PASS : EXIT_FAIL;
}
