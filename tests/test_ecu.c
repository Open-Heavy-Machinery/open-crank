/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_ecu.c
 * @brief The reference engine-ECU logic (portable/engine_ecu): its geometry
 *        against the Level 2 plant's, synchronisation on a 60-2 wheel and a
 *        cam (clean, mid-revolution start, missing or extra tooth, missing or
 *        misplaced cam, stop), speed, where each injection is armed and where
 *        it lands, the governor, TSC1, EEC1, and a closed speed loop.
 *
 * The edges come from a wheel model in this file: exact angles, constant
 * speed, times in ticks of a 170 MHz 32-bit timer that starts near its wrap.
 */

#include <math.h>
#include <string.h>

#include "check.h"
#include <opencrank/ecu.h>
#include <opencrank/plants/d936la6.h>

static const EcuParams *const P = &ECU_DEFAULT_PARAMS;
static const double TICK_HZ = 170e6;

/* ---- a wheel model -------------------------------------------------------- */

typedef struct {
    double rpm;
    double angle;  /* unwrapped crank degrees of the last event, 0 = TDC 1 of cycle 0 */
    double t;      /* ticks of the last event, unwrapped */
    double t0;     /* the timer's value at t = 0 (it wraps) */
    int real;      /* real teeth per revolution (58 for 60-2) */
    double pitch;  /* degrees between teeth (6 for 60-2) */
    int drop_tooth_at;  /* edge number to drop (-1: none) */
    int extra_tooth_at; /* edge number 2 degrees after which a spurious edge comes (-1: none) */
    double extra_at;
    int cam_mode;       /* 0 normal, 1 none, 2 at 200 degrees, 3 none for cycles [cam_from, cam_to) */
    int cam_from, cam_to;
    int edges;
    int tooth_due; /* the cam came first at an angle that also has a tooth */
    int fill_rev;  /* the revolution whose gap gets its two teeth back (-1: none) */
} Wheel;

typedef struct {
    int is_cam;
    double angle;
    uint32_t t;
} Event;

static void wheel_start(Wheel *g, double rpm, double angle) {
    memset(g, 0, sizeof(*g));
    g->rpm = rpm;
    g->angle = angle;
    g->t0 = 4294967296.0 - 3e6; /* the timer wraps about 18 ms in */
    g->real = 58;
    g->pitch = 6.0;
    g->drop_tooth_at = -1;
    g->extra_tooth_at = -1;
    g->extra_at = -1.0;
    g->fill_rev = -1;
}

static double deg_to_ticks(const Wheel *g, double deg) {
    return deg / (g->rpm * 6.0) * TICK_HZ;
}

static uint32_t timer(const Wheel *g, double t) {
    return (uint32_t)fmod(g->t0 + t, 4294967296.0);
}

/* The first tooth edge strictly after angle a; the first tooth begins at -60 every revolution. */
static double next_tooth(const Wheel *g, double a) {
    const double base = floor((a + 60.0) / 360.0) * 360.0 - 60.0;
    const double i = floor((a - base) / g->pitch + 1e-9) + 1.0;
    const int rev = (int)floor((base + 60.0) / 360.0 + 1e-9);
    const double teeth = (rev == g->fill_rev) ? 60.0 : (double)g->real;
    return (i >= teeth) ? base + 360.0 : base + g->pitch * i;
}

static double next_cam(const Wheel *g, double a) {
    const double at = (g->cam_mode == 2) ? 200.0 : 90.0;
    double c = floor(a / 720.0) * 720.0 + at;
    if (c <= a + 1e-9) {
        c += 720.0;
    }
    return c;
}

static int cam_wanted(const Wheel *g, double a) {
    const int cycle = (int)floor(a / 720.0);
    if (g->cam_mode == 1) {
        return 0;
    }
    return !(g->cam_mode == 3 && cycle >= g->cam_from && cycle < g->cam_to);
}

static void wheel_move(Wheel *g, double to) {
    g->t += deg_to_ticks(g, to - g->angle);
    g->angle = to;
}

/* The next event. A cam and a tooth at the same angle (tooth 25 at 90 degrees): the cam first. */
static Event wheel_next(Wheel *g) {
    Event e = { 0, 0.0, 0 };
    for (;;) {
        if (g->tooth_due) {
            g->tooth_due = 0;
            g->edges++;
            if (g->edges != g->drop_tooth_at) {
                break;
            }
            continue;
        }
        const double tooth = next_tooth(g, g->angle);
        const double cam = next_cam(g, g->angle);
        const double extra = (g->extra_at > g->angle) ? g->extra_at : 1e300;
        if (extra < tooth && extra < cam) {
            wheel_move(g, extra);
            g->extra_at = -1.0;
            break;
        }
        if (cam <= tooth + 1e-9) {
            wheel_move(g, cam);
            if (!cam_wanted(g, cam)) {
                g->tooth_due = fabs(cam - tooth) < 1e-9;
                continue;
            }
            g->tooth_due = fabs(cam - tooth) < 1e-9;
            e.is_cam = 1;
            break;
        }
        wheel_move(g, tooth);
        g->edges++;
        if (g->edges == g->extra_tooth_at) {
            g->extra_at = tooth + 2.0;
        }
        if (g->edges != g->drop_tooth_at) {
            break;
        }
    }
    e.angle = g->angle;
    e.t = timer(g, g->t);
    return e;
}

typedef struct {
    EcuWheel w;
    int arms;
    int arm_wrong;     /* an injection on the wrong injector, or off its angle */
    double worst_err;  /* degrees */
    int pending;       /* an arm waiting for the next tooth */
    EcuArm arm;
    int full_at_edge;  /* the edge number at which FULL was first reached, -1 */
    double start_angle;
} Run;

/* Feeds @p n events; checks each injection where the next tooth's edge plus its delay puts it. */
static void feed(Run *r, Wheel *g, int n) {
    for (int k = 0; k < n; k++) {
        const Event e = wheel_next(g);
        if (e.is_cam) {
            ecu_wheel_cam(&r->w, e.t);
            continue;
        }
        if (r->pending) {
            r->pending = 0;
            /* where this injection starts: this edge + delay */
            const double start = e.angle + (double)r->arm.delay_ticks / TICK_HZ * g->rpm * 6.0;
            const double soi = oc_l2_tdc_deg(r->arm.injector) - 10.0;
            double err = fmod(start - soi, 720.0);
            if (err > 360.0) {
                err -= 720.0;
            } else if (err < -360.0) {
                err += 720.0;
            }
            if (fabs(err) > r->worst_err) {
                r->worst_err = fabs(err);
            }
            if (fabs(err) > 0.05) {
                r->arm_wrong++;
            }
        }
        EcuArm a;
        if (ecu_wheel_tooth(&r->w, e.t, &a)) {
            r->arms++;
            r->pending = 1;
            r->arm = a;
        }
        if (r->full_at_edge < 0 && r->w.st.sync == ECU_SYNC_FULL) {
            r->full_at_edge = g->edges;
        }
    }
}

static void run_start(Run *r, uint32_t width) {
    memset(r, 0, sizeof(*r));
    ecu_wheel_init(&r->w, P);
    ecu_wheel_set_width(&r->w, width);
    r->full_at_edge = -1;
}

/* ---- tests ---------------------------------------------------------------- */

/* The ECU's geometry and calibration are the Level 2 plant's and Level 1's. */
static void test_same_geometry_as_the_plant(void) {
    CHECK(P->tooth0_deg == (float)OC_L2_TOOTH0_DEG);
    CHECK(P->cam_deg == (float)OC_L2_CAM_START_DEG);
    for (unsigned c = 1; c <= 6u; c++) {
        CHECK(P->tdc_deg[c - 1u] == (float)oc_l2_tdc_deg(c));
    }
    CHECK(P->injector_dead_us == (float)L2_D936LA6.injector_dead_us);
    CHECK(P->injector_full_us == (float)L2_D936LA6.injector_full_us);
    const EngineEcuParams *l1 = &ENGINE_ECU_D936LA6;
    CHECK(P->low_idle_rpm == (float)l1->low_idle_rpm && P->high_idle_rpm == (float)l1->high_idle_rpm);
    CHECK(P->overspeed_trip_rpm == (float)l1->overspeed_trip_rpm && P->running_rpm == (float)l1->running_rpm);
    CHECK(P->start_fuel_pct == (float)l1->start_fuel_pct);
    CHECK(P->kp_pct_per_rpm == (float)l1->kp_pct_per_rpm && P->ki_pct_per_rpm_s == (float)l1->ki_pct_per_rpm_s);
    CHECK(P->request_timeout_ms == l1->request_timeout_ms && P->source_address == l1->source_address);
}

/* Each cylinder is armed on the tooth before its reference tooth, 8 degrees before its start of injection. */
static void test_arm_points(void) {
    EcuWheel w;
    ecu_wheel_init(&w, P);
    /* cylinders 1..6: TDC 0, 480, 240, 600, 120, 360 */
    const uint8_t tooth[6] = { 6, 26, 46, 46, 26, 6 };
    const uint8_t rev[6] = { 0, 1, 0, 1, 0, 1 };
    for (unsigned c = 0; c < 6u; c++) {
        CHECK(w.arm[c].arm_tooth == tooth[c]);
        CHECK(w.arm[c].arm_rev == rev[c]);
        CHECK_NEAR(w.arm[c].delay_deg, 8.0, 1e-4);
    }
}

/* A clean wheel from TDC 1: FULL within three revolutions, then the right tooth, revolution and speed. */
static void test_clean_sync(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, -1.0);
    run_start(&r, 0u);
    feed(&r, &g, 3000);
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
    CHECK(r.full_at_edge > 0 && r.full_at_edge <= 3 * 58);
    CHECK(r.w.st.sync_losses == 0u);
    CHECK_NEAR(r.w.st.rpm, 1500.0, 1.5);
    /* the last event was an edge or a cam at g.angle: its tooth index and revolution */
    double rel = fmod(g.angle + 60.0, 720.0);
    const unsigned rev = rel >= 360.0 ? 1u : 0u;
    const unsigned idx = (unsigned)floor(fmod(rel, 360.0) / 6.0 + 1e-9);
    CHECK(r.w.st.rev == rev);
    CHECK(r.w.st.tooth == idx);
}

/* From any angle, FULL within three revolutions of the first edge, and the injections then land on their angle. */
static void test_sync_from_any_angle(void) {
    int worst = 0;
    int all_full = 1, no_loss = 1, landed = 1;
    for (int a = 0; a < 720; a += 5) {
        Wheel g;
        Run r;
        wheel_start(&g, 200.0, (double)a + 0.5); /* cranking speed */
        run_start(&r, 51000u);
        feed(&r, &g, 700);
        all_full &= (r.w.st.sync == ECU_SYNC_FULL);
        no_loss &= (r.w.st.sync_losses == 0u);
        landed &= (r.arm_wrong == 0 && r.arms >= 6);
        if (r.full_at_edge > worst) {
            worst = r.full_at_edge;
        }
    }
    CHECK(all_full);
    CHECK(no_loss);
    CHECK(landed);
    CHECK(worst > 0 && worst <= 3 * 58);
    printf("  slowest start: FULL at edge %d (limit %d, three revolutions)\n", worst, 3 * 58);
}

/* With a width, every injection is armed on its own injector and lands on its start of injection. */
static void test_injections_land_on_their_angle(void) {
    const double speeds[] = { 200.0, 750.0, 1500.0, 2400.0 };
    for (unsigned i = 0; i < 4u; i++) {
        Wheel g;
        Run r;
        wheel_start(&g, speeds[i], 0.0);
        run_start(&r, 51000u);
        feed(&r, &g, 4 * 2 * 62); /* about 4 cycles: edges and cams */
        CHECK(r.arms >= 12);
        CHECK(r.arm_wrong == 0);
        CHECK(r.worst_err < 0.05);
        CHECK(r.arm.width_ticks == 51000u);
    }
    /* no width, no injection */
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    run_start(&r, 0u);
    feed(&r, &g, 600);
    CHECK(r.arms == 0);
}

/* No injection before FULL. */
static void test_no_injection_before_full(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 100.0);
    g.cam_mode = 1; /* no cam: CRANK at most */
    run_start(&r, 51000u);
    feed(&r, &g, 1000);
    CHECK(r.w.st.sync == ECU_SYNC_CRANK);
    CHECK(r.arms == 0);
}

/* A missing tooth loses sync; it comes back within three revolutions. */
static void test_missing_tooth(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.drop_tooth_at = 400;
    run_start(&r, 0u);
    feed(&r, &g, 430);
    CHECK(r.w.st.sync_losses == 1u);
    CHECK(r.w.st.sync != ECU_SYNC_FULL);
    r.full_at_edge = -1;
    const int edges_at_loss = g.edges;
    feed(&r, &g, 600);
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
    CHECK(r.full_at_edge > 0 && r.full_at_edge - edges_at_loss <= 3 * 58);
    CHECK(r.w.st.sync_losses == 1u);
}

/* An extra edge loses sync too. */
static void test_extra_tooth(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.extra_tooth_at = 410;
    run_start(&r, 0u);
    feed(&r, &g, 500);
    CHECK(r.w.st.sync_losses == 1u);
    feed(&r, &g, 600);
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
}

/* One cycle without a cam is tolerated; two in a row lose sync. */
static void test_cam_missing(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.cam_mode = 3;
    g.cam_from = 4;
    g.cam_to = 5; /* one cycle */
    run_start(&r, 0u);
    feed(&r, &g, 1200);
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
    CHECK(r.w.st.sync_losses == 0u);

    wheel_start(&g, 1500.0, 0.0);
    g.cam_mode = 3;
    g.cam_from = 4;
    g.cam_to = 6; /* two cycles */
    run_start(&r, 0u);
    feed(&r, &g, 1200);
    CHECK(r.w.st.sync_losses == 1u);
    CHECK(r.w.st.sync == ECU_SYNC_FULL); /* the cam came back */
}

/* A wheel with 57 real teeth (59-2): its gaps are 57 teeth apart, never 58, so never CRANK. */
static void test_wrong_tooth_count(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.real = 57;
    g.pitch = 360.0 / 59.0;
    run_start(&r, 51000u);
    feed(&r, &g, 2000);
    CHECK(r.w.st.gaps > 10u);
    CHECK(r.w.st.sync == ECU_SYNC_SEEKING);
    CHECK(r.arms == 0);
}

/* A gap of two pitches (a 59-1 wheel with 58 real teeth) is not the 60-2 wheel's gap of three. */
static void test_two_pitch_gap_is_not_a_gap(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.pitch = 360.0 / 59.0;
    run_start(&r, 51000u);
    feed(&r, &g, 2000);
    CHECK(r.w.st.gaps == 0u);
    CHECK(r.w.st.sync == ECU_SYNC_SEEKING);
}

/* A revolution whose gap is filled (two teeth too many) loses sync at once, at the gap. */
static void test_filled_gap(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.fill_rev = 6;
    run_start(&r, 51000u);
    /* up to 10 degrees into revolution 7: past the filled gap, far from the next one */
    while (g.angle < 7.0 * 360.0 - 60.0 + 10.0) {
        feed(&r, &g, 1);
    }
    CHECK(r.w.st.sync_losses == 1u);
    CHECK(r.w.st.sync != ECU_SYNC_FULL);
}

/* A cam out of its window never gives FULL. */
static void test_cam_out_of_window(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    g.cam_mode = 2;
    run_start(&r, 51000u);
    feed(&r, &g, 1500);
    CHECK(r.w.st.sync == ECU_SYNC_CRANK);
    CHECK(r.arms == 0);
}

/* No tooth for 100 ms is a stop. */
static void test_stop(void) {
    Wheel g;
    Run r;
    wheel_start(&g, 1500.0, 0.0);
    run_start(&r, 0u);
    feed(&r, &g, 500);
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
    const uint32_t last = timer(&g, g.t);
    ecu_wheel_poll(&r.w, last + (uint32_t)(0.090 * TICK_HZ));
    CHECK(r.w.st.sync == ECU_SYNC_FULL);
    ecu_wheel_poll(&r.w, last + (uint32_t)(0.101 * TICK_HZ));
    CHECK(r.w.st.sync == ECU_SYNC_NONE);
    CHECK(r.w.st.stops == 1u && r.w.st.sync_losses == 0u);
    CHECK(r.w.st.teeth > 400u); /* counters kept */
}

/* The speed over 120 degrees, right after the gap too, across the timer's wrap. */
static void test_speed(void) {
    const double speeds[] = { 150.0, 750.0, 2100.0, 6000.0 };
    for (unsigned i = 0; i < 4u; i++) {
        Wheel g;
        Run r;
        wheel_start(&g, speeds[i], 0.0);
        run_start(&r, 0u);
        double worst = 0.0;
        for (int k = 0; k < 400; k++) {
            feed(&r, &g, 1);
            if (k > 30) {
                worst = fmax(worst, fabs(r.w.st.rpm - speeds[i]) / speeds[i]);
            }
        }
        CHECK(worst < 1e-4);
    }
}

/* ---- the governor ---------------------------------------------------------- */

static EcuWheelStatus status(EcuSync sync, float rpm) {
    EcuWheelStatus s;
    memset(&s, 0, sizeof(s));
    s.sync = sync;
    s.rpm = rpm;
    return s;
}

static J1939Frame tsc1(uint8_t mode, float rpm, uint8_t counter, uint8_t src) {
    J1939Tsc1 m;
    memset(&m, 0, sizeof(m));
    m.override_mode = (J1939OverrideMode)mode;
    m.mode_priority = 3u;
    m.requested_speed_valid = true;
    m.requested_speed_rpm = rpm;
    m.message_counter = counter;
    J1939Frame f;
    j1939_encode_tsc1(&m, src, 0x00u, &f);
    return f;
}

static uint32_t step(EcuCtx *e, EcuSync sync, float rpm, uint32_t now, J1939Eec1 *eec1) {
    const EcuWheelStatus s = status(sync, rpm);
    J1939Frame out[2];
    const size_t n = ecu_step(e, &s, now, out, 2u);
    CHECK(n == 1u);
    CHECK(j1939_id_source(out[0].id) == 0x00u);
    CHECK(j1939_decode_eec1(&out[0], eec1));
    return ecu_width_ticks(e);
}

static void test_governor_start_and_idle(void) {
    EcuCtx e;
    ecu_init(&e, P);
    J1939Eec1 m;
    CHECK(step(&e, ECU_SYNC_CRANK, 200.0f, 0u, &m) == 0u); /* no fuel without FULL */
    CHECK(e.state == ECU_GOV_STOPPED);
    const uint32_t w = step(&e, ECU_SYNC_FULL, 200.0f, 10u, &m);
    CHECK(e.state == ECU_GOV_CRANKING);
    CHECK_NEAR(e.fuel_pct, 30.0, 1e-4);
    CHECK_NEAR((double)w, (200.0 + 1600.0 * 0.30) * 170.0, 2.0); /* 680 us */
    CHECK_NEAR(m.engine_speed_rpm, 200.0, 0.2);
    CHECK(m.torque_mode == J1939_TORQUE_MODE_LOW_IDLE);
    (void)step(&e, ECU_SYNC_FULL, 600.0f, 20u, &m);
    CHECK(e.state == ECU_GOV_RUNNING);
    CHECK_NEAR(e.target_rpm, 750.0, 1e-4);
    CHECK(e.fuel_pct > 30.0f); /* below idle: more fuel */
    (void)step(&e, ECU_SYNC_SEEKING, 600.0f, 30u, &m);
    CHECK(e.state == ECU_GOV_STOPPED && ecu_width_ticks(&e) == 0u);
}

static void test_tsc1(void) {
    EcuCtx e;
    ecu_init(&e, P);
    J1939Eec1 m;
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 0u, &m);
    J1939Frame f = tsc1(1u, 1500.0f, 1u, 0x27u);
    ecu_handle_frame(&e, &f, 5u);
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 10u, &m);
    CHECK_NEAR(e.target_rpm, 1500.0, 1e-3);
    CHECK(m.controlling_address == 0x27u && m.torque_mode == J1939_TORQUE_MODE_NA);

    /* the same counter again, then a bad checksum: rejected */
    ecu_handle_frame(&e, &f, 20u);
    J1939Frame bad = tsc1(1u, 1800.0f, 2u, 0x27u);
    bad.data[1] ^= 0x01u;
    ecu_handle_frame(&e, &bad, 20u);
    CHECK(e.tsc1_rejected == 2u);
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 30u, &m);
    CHECK_NEAR(e.target_rpm, 1500.0, 1e-3);

    /* above high idle: clamped */
    f = tsc1(1u, 2500.0f, 3u, 0x27u);
    ecu_handle_frame(&e, &f, 40u);
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 40u, &m);
    CHECK_NEAR(e.target_rpm, 2100.0, 1e-3);

    /* stale after 200 ms: low idle, the ECU in control */
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 241u, &m);
    CHECK_NEAR(e.target_rpm, 750.0, 1e-3);
    CHECK(m.controlling_address == 0x00u);

    /* override disabled: low idle */
    f = tsc1(0u, 1500.0f, 4u, 0x27u);
    ecu_handle_frame(&e, &f, 250u);
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 250u, &m);
    CHECK_NEAR(e.target_rpm, 750.0, 1e-3);

    /* to another ECU: ignored */
    J1939Tsc1 o;
    memset(&o, 0, sizeof(o));
    o.override_mode = J1939_OVERRIDE_SPEED;
    o.requested_speed_valid = true;
    o.requested_speed_rpm = 1500.0f;
    o.message_counter = 5u;
    j1939_encode_tsc1(&o, 0x27u, 0x01u, &f);
    ecu_handle_frame(&e, &f, 260u);
    (void)step(&e, ECU_SYNC_FULL, 800.0f, 260u, &m);
    CHECK_NEAR(e.target_rpm, 750.0, 1e-3);
}

static void test_overspeed(void) {
    EcuCtx e;
    ecu_init(&e, P);
    J1939Eec1 m;
    (void)step(&e, ECU_SYNC_FULL, 1500.0f, 0u, &m);
    CHECK(ecu_width_ticks(&e) > 0u);
    CHECK(step(&e, ECU_SYNC_FULL, 2450.0f, 10u, &m) == 0u);
    CHECK(e.overspeed_tripped);
    CHECK(step(&e, ECU_SYNC_FULL, 1000.0f, 20u, &m) == 0u); /* latched */
    (void)step(&e, ECU_SYNC_NONE, 0.0f, 30u, &m);          /* stopped */
    CHECK(!e.overspeed_tripped);
    CHECK(step(&e, ECU_SYNC_FULL, 200.0f, 40u, &m) > 0u);
}

/* Anti-windup: held below its target at full fuel, the integrator stops, so reaching the target brings the fuel down. */
static void test_anti_windup(void) {
    EcuCtx e;
    ecu_init(&e, P);
    J1939Eec1 m;
    uint8_t counter = 0u;
    for (uint32_t ms = 0u; ms <= 5000u; ms += 10u) {
        J1939Frame f = tsc1(1u, 1500.0f, counter, 0x27u);
        counter = (uint8_t)((counter + 1u) & 0x0Fu);
        ecu_handle_frame(&e, &f, ms);
        (void)step(&e, ECU_SYNC_FULL, 600.0f, ms, &m); /* 900 rpm short: fuel at 100 % */
    }
    CHECK_NEAR(e.fuel_pct, 100.0, 1e-3);
    J1939Frame f = tsc1(1u, 1500.0f, counter, 0x27u);
    ecu_handle_frame(&e, &f, 5010u);
    (void)step(&e, ECU_SYNC_FULL, 1500.0f, 5010u, &m);
    CHECK(e.fuel_pct < 20.0f);
}

/* The governor holds a requested speed on the d936la6 plant, 10 ms steps, with the plant's mean fuel. */
static void test_closed_loop_holds_speed(void) {
    EcuCtx e;
    ecu_init(&e, P);
    PlantState s;
    plant_init(&s, &PLANT_D936LA6);
    uint8_t counter = 0u;
    double worst_after_settle = 0.0;
    for (uint32_t ms = 0u; ms < 20000u; ms += 10u) {
        const bool starter = ms < 2500u;
        if (ms >= 3000u) {
            J1939Frame f = tsc1(1u, 1500.0f, counter, 0x27u);
            counter = (uint8_t)((counter + 1u) & 0x0Fu);
            ecu_handle_frame(&e, &f, ms);
        }
        J1939Eec1 m;
        const EcuSync sync = (s.rpm > 50.0) ? ECU_SYNC_FULL : ECU_SYNC_SEEKING;
        (void)step(&e, sync, (float)s.rpm, ms, &m);
        const PlantInputs in = { .fuel_pct = e.fuel_pct, .load_torque_nm = (ms >= 12000u) ? 400.0 : 0.0,
                                 .starter_engaged = starter };
        plant_step(&s, &PLANT_D936LA6, &in, 0.010);
        if ((ms >= 9000u && ms < 12000u) || ms >= 18000u) {
            worst_after_settle = fmax(worst_after_settle, fabs(s.rpm - 1500.0));
        }
    }
    CHECK(worst_after_settle < 7.5); /* +-0.5 % */
}

int main(void) {
    RUN_TEST(test_same_geometry_as_the_plant);
    RUN_TEST(test_arm_points);
    RUN_TEST(test_clean_sync);
    RUN_TEST(test_sync_from_any_angle);
    RUN_TEST(test_injections_land_on_their_angle);
    RUN_TEST(test_no_injection_before_full);
    RUN_TEST(test_missing_tooth);
    RUN_TEST(test_extra_tooth);
    RUN_TEST(test_cam_missing);
    RUN_TEST(test_wrong_tooth_count);
    RUN_TEST(test_two_pitch_gap_is_not_a_gap);
    RUN_TEST(test_filled_gap);
    RUN_TEST(test_cam_out_of_window);
    RUN_TEST(test_stop);
    RUN_TEST(test_speed);
    RUN_TEST(test_governor_start_and_idle);
    RUN_TEST(test_tsc1);
    RUN_TEST(test_overspeed);
    RUN_TEST(test_anti_windup);
    RUN_TEST(test_closed_loop_holds_speed);
    return check_report();
}
