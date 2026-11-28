/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file ecu_check_main.c
 * @brief open-crank-ecu-check: where the engine ECU's injections land, on the
 *        board, at a constant crank speed (Level 2, v0.4 step 6). No plant.
 *
 *   open-crank-ecu-check --dev <Tang UART> --rpm R [--seconds S] [--warmup W]
 *                        [--captures c.csv] [--jitter j.csv] [--budget b.csv]
 *                        [--bitstream-md5 M] [--commit C]
 *
 * It drives the Tang Nano 20K's bridge through open-tick's hard loop
 * (hard_loop.h) with the Level 2 wheel (oc_l2_bridge_config(): 60-2, tooth 0
 * at -60 degrees, cam 90-120 degrees, six active-high captures) at one
 * constant `crank_phase_inc` for warmup + seconds + 0.5 s. The NUCLEO-G431KB
 * runs oc-stm32g4-ecu-fixedfuel (20 % fuel, 520 us) and injects; the bridge
 * captures every pulse.
 *
 * The angle of each injection's start comes from the bridge's own
 * accumulator, as the Level 2 plant computes it (sim/l2): from a freshly
 * loaded bitstream it is 0 at step 0 and gains `inc` every tick, so at a
 * capture's tick t it is inc x (t - t_start), mod 2^48 = 720 degrees.
 *
 * Criteria (each printed with its figure; exit 1 if one fails):
 *   - per channel, the injections that start inside [warmup, warmup + seconds)
 *     are rpm / 120 x seconds, +-1;
 *   - every capture is on its cylinder's channel, its start within 0.5 degrees
 *     of that cylinder's TDC - 10 degrees; p99 of |error| <= 0.1 degrees
 *     (decision 14);
 *   - every width is 520 us x (1 + 0.27 %) +- 1 us (the NUCLEO's HSI, F-22);
 *   - no capture lost (the bridge's overflow, or the loop's queue).
 * The USART2 line (sync FULL) and EEC1 are tools/board/ecucheck.py's.
 *
 * Exit 0 all criteria met, 1 a criterion failed, 2 usage, 3 the hard run was
 * not ok (aborted or degraded), 4 a file could not be written.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opencrank/ecu.h>
#include <opencrank/l2/plant_l2.h>
#include <opentick/host/hard_loop.h>

#define ACC_MASK ((UINT64_C(1) << 48) - 1u)
#define WIDTH_US 520.0          /* 20 % fuel through the injector model */
#define HSI_BIAS 0.0027         /* the ECU's timers run 0.27 % slow (F-22): widths come out longer */
#define WIDTH_TOL_US 1.0
#define SOI_MAX_DEG 0.5
#define SOI_P99_DEG 0.1
#define TAIL_S 0.5

typedef struct {
    uint8_t channel;
    uint64_t start_tick;
    uint32_t width;
} Cap;

static Cap *g_cap;
static size_t g_n, g_capn;
static OtkHardLoop g_hard;

static bool keep(const OtkHardCapture *c) {
    if (g_n == g_capn) {
        const size_t n = g_capn ? 2u * g_capn : 4096u;
        Cap *p = realloc(g_cap, n * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        g_cap = p;
        g_capn = n;
    }
    g_cap[g_n].channel = c->channel;
    g_cap[g_n].start_tick = c->start_tick;
    g_cap[g_n].width = c->width;
    g_n++;
    return true;
}

static bool drain(void) {
    OtkHardCapture buf[64];
    size_t n;
    while ((n = otk_hard_loop_captures(&g_hard, buf, 64u)) != 0u) {
        for (size_t i = 0; i < n; i++) {
            if (!keep(&buf[i])) {
                return false;
            }
        }
    }
    return true;
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

static void usage(void) {
    fprintf(stderr, "usage: open-crank-ecu-check --dev <Tang UART> --rpm R [--seconds S] [--warmup W] "
                    "[--captures c.csv] [--jitter j.csv] [--budget b.csv] [--bitstream-md5 M] [--commit C]\n");
}

static void budget_row(FILE *fp, const char *q, const char *declared, double measured, const char *unit,
                       double limit) {
    fprintf(fp, "%s,%s,%.6f,%s,", q, declared, measured, unit);
    if (limit >= 0.0) {
        fprintf(fp, "%s", measured <= limit ? "yes" : "no");
    }
    fprintf(fp, "\n");
}

int main(int argc, char **argv) {
    const char *dev = NULL, *cap_path = NULL, *jitter_path = NULL, *budget_path = NULL;
    const char *md5 = "", *commit = "";
    double rpm = 0.0, seconds = 30.0, warmup = 2.0;
    for (int i = 1; i < argc; i += 2) { /* every option takes a value */
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (v == NULL) {
            usage();
            return 2;
        }
        if (!strcmp(a, "--dev")) { dev = v; }
        else if (!strcmp(a, "--rpm")) { rpm = atof(v); }
        else if (!strcmp(a, "--seconds")) { seconds = atof(v); }
        else if (!strcmp(a, "--warmup")) { warmup = atof(v); }
        else if (!strcmp(a, "--captures")) { cap_path = v; }
        else if (!strcmp(a, "--jitter")) { jitter_path = v; }
        else if (!strcmp(a, "--budget")) { budget_path = v; }
        else if (!strcmp(a, "--bitstream-md5")) { md5 = v; }
        else if (!strcmp(a, "--commit")) { commit = v; }
        else {
            usage();
            return 2;
        }
    }
    if (dev == NULL || !(rpm > 0.0 && rpm <= 6000.0) || !(seconds >= 1.0) || !(warmup >= 0.5)) {
        usage();
        return 2;
    }

    OtkHardLoopConfig hc;
    otk_hard_loop_defaults(&hc, OC_L2_STEP_TICKS);
    oc_l2_bridge_config(&hc.bridge);
    hc.need_tick_hz = OC_L2_TICK_HZ;
    hc.need_capture = (uint8_t)OC_L2_CYLINDERS;
    char err[256];
    if (otk_hard_loop_open(&g_hard, dev, &hc, err, sizeof(err)) != 0) {
        fprintf(stderr, "open-crank-ecu-check: %s: %s\n", dev, err);
        return 3;
    }
    /* the bridge's formula (docs/bridge-protocol.md §3.2), as sim/l2 sends it */
    const uint32_t inc = (uint32_t)llround(rpm / 120.0 * 281474976710656.0 / (double)OC_L2_TICK_HZ);
    OtkBridgeSetpoint sp;
    memset(&sp, 0, sizeof(sp));
    sp.crank_phase_inc = inc;
    const uint32_t steps = (uint32_t)llround((warmup + seconds + TAIL_S) * 1000.0);
    printf("open-crank-ecu-check: %.0f rpm (crank_phase_inc %u) for %.1f s, judged over [%.1f, %.1f) s\n", rpm, inc,
           (double)steps / 1000.0, warmup, warmup + seconds);
    bool oom = false;
    for (uint32_t k = 0; k < steps; k++) {
        otk_hard_loop_set(&g_hard, &sp);
        if (!otk_hard_loop_next(&g_hard)) {
            break;
        }
        oom = oom || !drain();
    }
    const OtkHardResult result = otk_hard_loop_stop(&g_hard);
    oom = oom || !drain();
    const uint64_t t_start = g_hard.aim.t_start;
    const OtkHardStats *st = &g_hard.stats;
    const uint32_t lost = st->capture_lost + g_hard.cap_dropped;
    printf("hard run %s: %u steps, %u underruns, %u late, %llu exchanges, %u timeouts, %u crc errors, "
           "%u sequence errors, round trip p99 %.0f us; %zu captures, %u lost\n",
           otk_hard_result_name(result), st->steps, st->underruns, st->late, (unsigned long long)st->exchanges,
           st->timeouts, st->crc_errors, st->seq_errors, (double)otk_hard_loop_rtt_pct(&g_hard, 99.0) / 1e3, g_n,
           lost);
    /* for tools/board/ecucheck.py: when step 0 began on the host's CLOCK_MONOTONIC */
    printf("step 0 at monotonic %llu ns\n", (unsigned long long)otk_hard_aim_step_host_ns(&g_hard.aim, 0u));
    if (oom) {
        fprintf(stderr, "open-crank-ecu-check: out of memory\n");
        otk_hard_loop_free(&g_hard);
        return 4;
    }

    /* ---- the captures, one by one ---- */
    FILE *cf = cap_path ? fopen(cap_path, "w") : NULL;
    if (cap_path && cf == NULL) {
        fprintf(stderr, "open-crank-ecu-check: cannot write %s\n", cap_path);
        return 4;
    }
    if (cf) {
        fprintf(cf, "# open-crank-ecu-check; rpm=%.0f; crank_phase_inc=%u; t_start=%llu; tick_hz=%u\n", rpm, inc,
                (unsigned long long)t_start, (unsigned)OC_L2_TICK_HZ);
        fprintf(cf, "channel,start_tick,t_s,width_ticks,width_us,angle_deg,soi_err_deg,in_window\n");
    }
    const double want_w = WIDTH_US * (1.0 + HSI_BIAS);
    double *errs = calloc(g_n ? g_n : 1u, sizeof(double));
    size_t n_err = 0;
    unsigned in_win[OC_L2_CYLINDERS] = { 0 };
    unsigned wrong_place = 0, before_start = 0, width_bad = 0;
    double soi_max = 0.0, soi_sum = 0.0, w_min = 1e9, w_max = 0.0, w_sum = 0.0;
    double werr_max = 0.0, wrel_max = 0.0, wexcess_max = -1e9;
    unsigned per_ch[OC_L2_CYLINDERS] = { 0 };
    for (size_t i = 0; i < g_n; i++) {
        const Cap *c = &g_cap[i];
        if (c->start_tick < t_start || c->channel >= OC_L2_CYLINDERS) {
            before_start++;
            continue;
        }
        const uint64_t since = c->start_tick - t_start;
        const double t_s = (double)since / (double)OC_L2_TICK_HZ;
        const double deg = oc_l2_acc_deg((inc * since) & ACC_MASK);
        const unsigned cyl = c->channel + 1u;
        double e = deg - (oc_l2_tdc_deg(cyl) - (double)ECU_DEFAULT_PARAMS.soi_btdc_deg);
        e = fmod(e + 1080.0, 720.0) - 360.0;
        const double w_us = (double)c->width * 1e6 / (double)OC_L2_TICK_HZ;
        const bool in_window = t_s >= warmup && t_s < warmup + seconds;
        per_ch[c->channel]++;
        if (in_window) {
            in_win[c->channel]++;
        }
        if (fabs(e) > SOI_MAX_DEG) {
            wrong_place++;
        }
        errs[n_err++] = fabs(e);
        soi_sum += e;
        if (fabs(e) > soi_max) {
            soi_max = fabs(e);
        }
        if (fabs(w_us - want_w) > WIDTH_TOL_US) {
            width_bad++;
        }
        w_min = fmin(w_min, w_us);
        w_max = fmax(w_max, w_us);
        w_sum += w_us;
        const double werr = fabs(w_us - WIDTH_US);
        werr_max = fmax(werr_max, werr);
        wrel_max = fmax(wrel_max, werr / WIDTH_US);
        wexcess_max = fmax(wexcess_max, werr - 0.003 * WIDTH_US);
        if (cf) {
            fprintf(cf, "%u,%llu,%.6f,%u,%.3f,%.4f,%.5f,%d\n", c->channel, (unsigned long long)c->start_tick, t_s,
                    c->width, w_us, deg, e, in_window ? 1 : 0);
        }
    }
    if (cf && fclose(cf) != 0) {
        fprintf(stderr, "open-crank-ecu-check: cannot write %s\n", cap_path);
        return 4;
    }
    qsort(errs, n_err, sizeof(double), cmp_double);
    const double p50 = n_err ? errs[(n_err - 1u) / 2u] : 0.0;
    const double p99 = n_err ? errs[(size_t)ceil(0.99 * (double)n_err) - 1u] : 0.0;
    const double expect = rpm / 120.0 * seconds;

    bool ok = true;
    printf("\ncriteria (%.0f rpm):\n", rpm);
    for (unsigned k = 0; k < OC_L2_CYLINDERS; k++) {
        const bool c_ok = fabs((double)in_win[k] - expect) <= 1.0;
        ok = ok && c_ok;
        printf("  %s  injector %u: %u injections in %.0f s (expected %.1f +-1), %u in the whole run\n",
               c_ok ? "ok  " : "FAIL", k + 1u, in_win[k], seconds, expect, per_ch[k]);
    }
    const bool place_ok = n_err > 0 && wrong_place == 0 && p99 <= SOI_P99_DEG;
    ok = ok && place_ok;
    printf("  %s  start of injection vs TDC - 10 deg: %zu injections, %u off by more than %.1f deg; |error| p50 %.4f, "
           "p99 %.4f (<= %.1f), max %.4f deg; mean error %+.4f deg\n",
           place_ok ? "ok  " : "FAIL", n_err, wrong_place, SOI_MAX_DEG, p50, p99, SOI_P99_DEG, soi_max,
           n_err ? soi_sum / (double)n_err : 0.0);
    const bool width_ok = n_err > 0 && width_bad == 0;
    ok = ok && width_ok;
    printf("  %s  width vs %.3f us +-%.0f: %u outside; min %.3f, mean %.3f, max %.3f us\n", width_ok ? "ok  " : "FAIL",
           want_w, WIDTH_TOL_US, width_bad, n_err ? w_min : 0.0, n_err ? w_sum / (double)n_err : 0.0, w_max);
    const bool lost_ok = lost == 0 && before_start == 0;
    ok = ok && lost_ok;
    printf("  %s  captures lost: %u (bridge overflow %u, queue %u); before step 0 or on an unknown channel: %u\n",
           lost_ok ? "ok  " : "FAIL", lost, st->capture_lost, g_hard.cap_dropped, before_start);

    int rc = 0;
    if (jitter_path) {
        FILE *fp = fopen(jitter_path, "w");
        if (fp == NULL) {
            rc = 4;
        } else {
            fprintf(fp, "# oc-l2-jitter 1; source=open-crank-ecu-check; rpm=%.0f; seconds=%.1f; warmup=%.1f; "
                        "width_us=%.0f; bitstream_md5=%s; commit=%s\n",
                    rpm, seconds, warmup, WIDTH_US, md5, commit);
            fprintf(fp, "quantity,declared,measured,unit,within\n");
            fprintf(fp, "injections,,%zu,count,\n", n_err);
            budget_row(fp, "soi_err_p50_deg", "", p50, "deg", -1.0);
            budget_row(fp, "soi_err_p99_deg", "0.1", p99, "deg", 0.1);
            budget_row(fp, "soi_err_max_deg", "0.5", soi_max, "deg", 0.5);
            budget_row(fp, "width_err_max_us", "", werr_max, "us", -1.0);
            budget_row(fp, "width_err_max_rel", "", wrel_max, "", -1.0);
            budget_row(fp, "width_err_over_0.3pct_us", "1", n_err ? wexcess_max : 0.0, "us", 1.0);
            if (fclose(fp) != 0) {
                rc = 4;
            }
        }
    }
    if (budget_path) {
        char usb[256];
        (void)otk_hard_usb_path(dev, usb, sizeof(usb));
        OtkHardBudget b;
        otk_hard_budget_none(&b);
        b.underruns = 0u;
        b.late = 0u;
        const OtkHardRunMeta meta = { "bl616-3mbaud", usb, md5, commit, "open-crank-ecu-check" };
        const OtkHardCanErr ce = { 0u, 0u, 0u, 0u };
        FILE *fp = fopen(budget_path, "w");
        const bool w = fp != NULL && otk_hard_budget_write(fp, &g_hard, result, &b, &meta, &ce);
        if (fp == NULL || fclose(fp) != 0 || !w) {
            rc = 4;
        }
    }
    free(errs);
    free(g_cap);
    otk_hard_loop_free(&g_hard);
    if (rc != 0) {
        fprintf(stderr, "open-crank-ecu-check: cannot write an output file\n");
        return rc;
    }
    if (result != OTK_HARD_OK) {
        printf("\nopen-crank-ecu-check: the hard run was %s\n", otk_hard_result_name(result));
        return 3;
    }
    printf("\nopen-crank-ecu-check: %s\n", ok ? "every criterion met" : "FAILED");
    return ok ? 0 : 1;
}
