/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_plant_l2.c
 * @brief The Level 2 plant: the wheel's bridge configuration, the angle
 *        against the bridge's accumulator tick by tick, the injector model,
 *        the cylinder windows, misfires, late captures, and fuel that turns
 *        the engine only when it lands in the right cylinder.
 */

#include <string.h>

#include "check.h"
#include <opencrank/plants/d936la6.h>

static const PlantParams *const PP = &PLANT_D936LA6;
static const OcL2Params *const P = &L2_D936LA6;

#define ACC_FULL 281474976710656.0 /* 2^48 */
#define ACC_MASK ((UINT64_C(1) << 48) - 1u)

static const OcL2Inputs NO_INPUT = { .load_torque_nm = 0.0, .starter_engaged = false };

/* A plant turning at about @p rpm, with no fuel, @p steps steps in. */
static void spun_up(OcL2Plant *s, double rpm, unsigned steps) {
    oc_l2_init(s, PP);
    s->plant.rpm = rpm;
    for (unsigned i = 0; i < steps; i++) {
        oc_l2_step(s, PP, P, &NO_INPUT);
    }
}

/* The first tick of step j at which the accumulator reaches @p deg, if step j reaches it. */
static int tick_at(const OcL2Plant *s, uint32_t j, double deg, uint64_t *tick) {
    const OcL2StepRecord *r = &s->hist[j % OC_L2_HISTORY];
    if (r->inc == 0u) {
        return 0;
    }
    double d = deg;
    while (d < 0.0) {
        d += 720.0;
    }
    while (d >= 720.0) {
        d -= 720.0;
    }
    const uint64_t target = (uint64_t)(d / 720.0 * ACC_FULL);
    const uint64_t dist = (target - r->acc) & ACC_MASK;
    const uint64_t n = (dist + r->inc - 1u) / r->inc;
    if (n >= OC_L2_STEP_TICKS) {
        return 0;
    }
    *tick = (uint64_t)j * OC_L2_STEP_TICKS + n;
    return 1;
}

/* The first tick, in the steps already computed and still in the history, at which the angle reaches @p deg. */
static int find_tick(const OcL2Plant *s, double deg, uint64_t *tick) {
    const uint32_t first = (s->step > OC_L2_HISTORY) ? s->step - OC_L2_HISTORY : 0u;
    for (uint32_t j = first; j < s->step; j++) {
        if (tick_at(s, j, deg, tick)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t width_ticks(double fuel_pct) {
    return (uint32_t)(oc_l2_width_us(P, fuel_pct) * 27.0 + 0.5);
}

/* The bridge is configured for the wheel of decision 6: TDC 1 is the start of tooth 10. */
static void test_bridge_config(void) {
    OtkBridgeConfig c;
    oc_l2_bridge_config(&c);
    CHECK(c.step_ticks == 27000u);
    CHECK(c.crank_teeth == 60u && c.crank_missing == 2u);
    CHECK(c.crank_offset == oc_l2_phase32(660.0));
    CHECK(c.cam_start == oc_l2_phase32(90.0) && c.cam_end == oc_l2_phase32(120.0));
    CHECK(c.capture_mask == 0x3Fu && c.capture_polarity == 0x3Fu);
    /* the gateware's tooth index (rotary.v): floor(((ph - offset) mod 2^31) * teeth / 2^31) */
    /* (crank_offset is rounded to one LSB of the top 32 bits, 1.7e-7 degrees) */
    const uint32_t rel = (oc_l2_phase32(0.001) - c.crank_offset) & 0x7FFFFFFFu;
    CHECK((uint64_t)rel * 60u / 0x80000000u == 10u);
    const uint32_t before = (oc_l2_phase32(-0.01) - c.crank_offset) & 0x7FFFFFFFu;
    CHECK((uint64_t)before * 60u / 0x80000000u == 9u);
    /* the gap: tooth 58 begins 72 degrees before TDC 1 */
    const uint32_t gap = (oc_l2_phase32(-72.0 + 0.01) - c.crank_offset) & 0x7FFFFFFFu;
    CHECK((uint64_t)gap * 60u / 0x80000000u == 58u);
    CHECK(oc_l2_tdc_deg(1) == 0.0 && oc_l2_tdc_deg(5) == 120.0 && oc_l2_tdc_deg(3) == 240.0);
    CHECK(oc_l2_tdc_deg(6) == 360.0 && oc_l2_tdc_deg(2) == 480.0 && oc_l2_tdc_deg(4) == 600.0);
}

/* The plant's angle is the bridge's accumulator, at every step boundary and mid-step. */
static void test_angle_is_the_bridges(void) {
    OcL2Plant s;
    oc_l2_init(&s, PP);
    const OcL2Inputs crank = { .load_torque_nm = 0.0, .starter_engaged = true };
    uint64_t bridge = 0u; /* the gateware's 48-bit phase: +inc every tick, from 0 at step 0 */
    int same = 1, mid_same = 1;
    for (uint32_t k = 0; k < 400u; k++) {
        if (s.acc != bridge) {
            same = 0;
        }
        const uint32_t inc = oc_l2_step(&s, PP, P, &crank);
        uint64_t at_mid = 0u;
        for (uint32_t t = 0; t < OC_L2_STEP_TICKS; t++) {
            if (t == 12345u) {
                at_mid = bridge;
            }
            bridge = (bridge + inc) & ACC_MASK;
        }
        if (k % 50u == 7u) {
            double deg = -1.0;
            /* channel 6 does not exist: the capture is misplaced, but its angle is reported */
            (void)oc_l2_capture(&s, P, 6u, (uint64_t)k * OC_L2_STEP_TICKS + 12345u, 100u, &deg);
            if (deg != oc_l2_acc_deg(at_mid)) {
                mid_same = 0;
            }
        }
    }
    CHECK(same);
    CHECK(mid_same);
    CHECK(s.acc == bridge);
    CHECK(s.plant.rpm > 100.0); /* the starter turned it: the increments were not all zero */
    CHECK(s.inc == 0u || s.inc > 1000u);
    double rpm_from_inc = otk_bridge_rpm(s.hist[(s.step - 1u) % OC_L2_HISTORY].inc, OC_L2_TICK_HZ);
    CHECK(rpm_from_inc > 100.0);
}

/* The injector model and its inverse. */
static void test_injector_model(void) {
    CHECK_NEAR(oc_l2_width_us(P, 0.0), 200.0, 1e-9);
    CHECK_NEAR(oc_l2_width_us(P, 100.0), 1800.0, 1e-9);
    CHECK_NEAR(oc_l2_width_us(P, 150.0), 1800.0, 1e-9);
    for (int f = 0; f <= 100; f += 5) {
        CHECK_NEAR(oc_l2_fuel_pct(P, oc_l2_width_us(P, f)), f, 1e-9);
    }
    CHECK(oc_l2_fuel_pct(P, 150.0) == 0.0);
    CHECK(oc_l2_fuel_pct(P, 5000.0) == 100.0);
}

/* Inside its window and on its channel, an injection fuels its cylinder; otherwise it is misplaced. */
static void test_windows(void) {
    OcL2Plant s;
    spun_up(&s, 1500.0, 120u);
    uint64_t t;

    CHECK(find_tick(&s, -10.0, &t));
    CHECK(oc_l2_capture(&s, P, 0u, t, width_ticks(50.0), NULL) == OC_L2_CAPTURE_COUNTED);
    CHECK_NEAR(s.cyl_fuel_pct[0], 50.0, 0.05);
    CHECK_NEAR(oc_l2_fuel_mean(&s), 50.0 / 6.0, 0.01);

    /* the same angle on cylinder 2's channel */
    CHECK(oc_l2_capture(&s, P, 1u, t, width_ticks(50.0), NULL) == OC_L2_CAPTURE_MISPLACED);
    /* between two windows (cylinder 1's closes at 20, cylinder 5's opens at 80) */
    CHECK(find_tick(&s, 60.0, &t));
    CHECK(oc_l2_capture(&s, P, 0u, t, width_ticks(50.0), NULL) == OC_L2_CAPTURE_MISPLACED);
    CHECK(oc_l2_capture(&s, P, 4u, t, width_ticks(50.0), NULL) == OC_L2_CAPTURE_MISPLACED);
    /* the window's edges */
    CHECK(find_tick(&s, 120.0 - 39.9, &t));
    CHECK(oc_l2_capture(&s, P, 4u, t, width_ticks(10.0), NULL) == OC_L2_CAPTURE_COUNTED);
    CHECK(find_tick(&s, 120.0 - 40.5, &t));
    CHECK(oc_l2_capture(&s, P, 4u, t, width_ticks(10.0), NULL) == OC_L2_CAPTURE_MISPLACED);
    CHECK(find_tick(&s, 240.0 + 19.9, &t));
    CHECK(oc_l2_capture(&s, P, 2u, t, width_ticks(10.0), NULL) == OC_L2_CAPTURE_COUNTED);
    CHECK(find_tick(&s, 240.0 + 20.5, &t));
    CHECK(oc_l2_capture(&s, P, 2u, t, width_ticks(10.0), NULL) == OC_L2_CAPTURE_MISPLACED);
    /* cylinder 4, the last TDC of the cycle */
    CHECK(find_tick(&s, 590.0, &t));
    CHECK(oc_l2_capture(&s, P, 3u, t, width_ticks(30.0), NULL) == OC_L2_CAPTURE_COUNTED);
    CHECK_NEAR(s.cyl_fuel_pct[3], 30.0, 0.05);

    CHECK(s.injections == 4u);
    CHECK(s.misplaced == 5u);
}

/* A window that closes with no counted injection drops its cylinder's fuel to zero. */
static void test_misfire(void) {
    OcL2Plant s;
    spun_up(&s, 1500.0, 100u); /* 100 ms at 1500 rpm: more than one 720-degree cycle */
    uint64_t t;
    CHECK(find_tick(&s, -10.0, &t));
    const uint32_t at = (uint32_t)(t / OC_L2_STEP_TICKS);
    CHECK(oc_l2_capture(&s, P, 0u, t, width_ticks(40.0), NULL) == OC_L2_CAPTURE_COUNTED);
    const uint32_t misfires = s.misfires;

    /* The window it fuelled closes (20 degrees) and is judged capture_delay_steps later: no misfire. */
    while (s.step < at + 40u) {
        oc_l2_step(&s, PP, P, &NO_INPUT);
    }
    CHECK_NEAR(s.cyl_fuel_pct[0], 40.0, 0.05);

    /* No injection in its next window: after that window closes and is judged, the fuel is gone. */
    while (s.step < at + 200u) {
        oc_l2_step(&s, PP, P, &NO_INPUT);
    }
    CHECK(s.cyl_fuel_pct[0] == 0.0);
    CHECK(s.misfires > misfires);
}

/* Cylinder 1's window wraps the cycle: an injection at 700 degrees belongs to the next cycle's TDC. */
static void test_window_wraps(void) {
    OcL2Plant s;
    spun_up(&s, 1500.0, 100u);
    uint64_t t;
    CHECK(find_tick(&s, 700.0, &t));
    const uint32_t at = (uint32_t)(t / OC_L2_STEP_TICKS);
    CHECK(oc_l2_capture(&s, P, 0u, t, width_ticks(25.0), NULL) == OC_L2_CAPTURE_COUNTED);
    /* through cylinder 1's next window close (40 degrees on, about 5 ms) and its judgement */
    while (s.step < at + 5u + P->capture_delay_steps + 2u) {
        oc_l2_step(&s, PP, P, &NO_INPUT);
    }
    CHECK_NEAR(s.cyl_fuel_pct[0], 25.0, 0.05);
}

/* A capture in a step not computed yet, or older than the history, is ignored. */
static void test_late_captures(void) {
    OcL2Plant s;
    spun_up(&s, 1500.0, 300u);
    CHECK(oc_l2_capture(&s, P, 0u, (uint64_t)s.step * OC_L2_STEP_TICKS, 100u, NULL) == OC_L2_CAPTURE_LATE);
    CHECK(oc_l2_capture(&s, P, 0u, (uint64_t)(s.step - OC_L2_HISTORY - 1u) * OC_L2_STEP_TICKS, 100u, NULL) ==
          OC_L2_CAPTURE_LATE);
    CHECK(oc_l2_capture(&s, P, 0u, (uint64_t)(s.step - 1u) * OC_L2_STEP_TICKS, 100u, NULL) !=
          OC_L2_CAPTURE_LATE);
    CHECK(s.late_captures == 2u);
}

/*
 * An ideal ECU in the test: every cylinder injected @p soi_deg from its own
 * TDC with @p fuel, on channel (cylinder + @p channel_shift) % 6, the capture
 * handed over 17 steps after it began (H = 16 and a round trip). Runs 3 s from
 * 800 rpm with no load and returns the final speed.
 */
static double run_injected(double fuel, double soi_deg, unsigned channel_shift, OcL2Plant *s) {
    spun_up(s, 800.0, 1u);
    enum { PEND = 64 };
    struct {
        uint64_t tick;
        unsigned ch;
        uint32_t due;
    } pend[PEND];
    unsigned n = 0u;
    for (uint32_t k = 1u; k < 3000u; k++) {
        const uint32_t j = s->step;
        oc_l2_step(s, PP, P, &NO_INPUT);
        for (unsigned c = 1; c <= 6u; c++) {
            uint64_t t;
            if (n < PEND && tick_at(s, j, oc_l2_tdc_deg(c) + soi_deg, &t)) {
                pend[n].tick = t;
                pend[n].ch = (c - 1u + channel_shift) % 6u;
                pend[n].due = j + 17u;
                n++;
            }
        }
        unsigned kept = 0u;
        for (unsigned i = 0; i < n; i++) {
            if (pend[i].due <= s->step) {
                (void)oc_l2_capture(s, P, pend[i].ch, pend[i].tick, width_ticks(fuel), NULL);
            } else {
                pend[kept++] = pend[i];
            }
        }
        n = kept;
    }
    return s->plant.rpm;
}

/* Fuel turns the engine only when it lands in the right cylinder: a wrong phase does not run. */
static void test_fuel_in_the_right_cylinder(void) {
    OcL2Plant right, none, wrong;
    const double rpm_right = run_injected(15.0, -10.0, 0u, &right);
    const double rpm_none = run_injected(0.0, -10.0, 0u, &none);
    const double rpm_wrong = run_injected(15.0, 350.0, 0u, &wrong); /* 360 degrees off: the gas-exchange TDC */
    OcL2Plant crossed;
    const double rpm_crossed = run_injected(15.0, -10.0, 1u, &crossed); /* every pulse on the next channel */

    CHECK(rpm_right > 900.0);
    CHECK(rpm_none < 700.0);
    CHECK(rpm_wrong < 700.0);
    CHECK(rpm_crossed < 700.0);
    CHECK(right.misplaced == 0u);
    CHECK(right.injections > 100u);
    CHECK(wrong.injections == 0u && wrong.misplaced > 20u); /* it stops within a second */
    CHECK(crossed.injections == 0u && crossed.misplaced > 20u);
    CHECK(right.late_captures == 0u);
    CHECK(right.misfires <= 12u); /* only the windows that closed before the first injections */
}

/* Same inputs, same outputs, bit for bit. */
static void test_deterministic(void) {
    OcL2Plant a, b;
    (void)run_injected(20.0, -10.0, 0u, &a);
    (void)run_injected(20.0, -10.0, 0u, &b);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

int main(void) {
    RUN_TEST(test_bridge_config);
    RUN_TEST(test_angle_is_the_bridges);
    RUN_TEST(test_injector_model);
    RUN_TEST(test_windows);
    RUN_TEST(test_misfire);
    RUN_TEST(test_window_wraps);
    RUN_TEST(test_late_captures);
    RUN_TEST(test_fuel_in_the_right_cylinder);
    RUN_TEST(test_deterministic);
    return check_report();
}
