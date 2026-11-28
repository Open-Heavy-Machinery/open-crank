/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plant_l2.c
 * @brief The Level 2 plant. See plant_l2.h for the model.
 */

#include <opencrank/l2/plant_l2.h>

#include <math.h>
#include <string.h>

#define ACC_BITS 48u
#define ACC_MASK ((UINT64_C(1) << ACC_BITS) - 1u)
#define ACC_FULL 281474976710656.0 /* 2^48 */
#define CYCLE_DEG 720.0

/* TDC of cylinders 1-6, firing order 1-5-3-6-2-4. */
static const double TDC_DEG[OC_L2_CYLINDERS] = { 0.0, 480.0, 240.0, 600.0, 120.0, 360.0 };

static double clamp(double v, double lo, double hi) {
    if (!(v >= lo)) { /* also catches NaN */
        return lo;
    }
    return (v > hi) ? hi : v;
}

double oc_l2_tdc_deg(unsigned cyl) {
    return (cyl >= 1u && cyl <= OC_L2_CYLINDERS) ? TDC_DEG[cyl - 1u] : 0.0;
}

uint32_t oc_l2_phase32(double deg) {
    double d = fmod(deg, CYCLE_DEG);
    if (d < 0.0) {
        d += CYCLE_DEG;
    }
    const double v = d / CYCLE_DEG * 4294967296.0 + 0.5;
    return (v >= 4294967296.0) ? 0u : (uint32_t)v;
}

double oc_l2_acc_deg(uint64_t acc) {
    return (double)(acc & ACC_MASK) / ACC_FULL * CYCLE_DEG;
}

void oc_l2_bridge_config(OtkBridgeConfig *c) {
    memset(c, 0, sizeof(*c));
    c->step_ticks = OC_L2_STEP_TICKS;
    c->crank_teeth = (uint8_t)OC_L2_CRANK_TEETH;
    c->crank_missing = (uint8_t)OC_L2_CRANK_MISSING;
    c->crank_offset = oc_l2_phase32(OC_L2_TOOTH0_DEG);
    c->cam_start = oc_l2_phase32(OC_L2_CAM_START_DEG);
    c->cam_end = oc_l2_phase32(OC_L2_CAM_END_DEG);
    c->capture_mask = (uint8_t)((1u << OC_L2_CYLINDERS) - 1u);
    c->capture_polarity = (uint8_t)((1u << OC_L2_CYLINDERS) - 1u);
    c->link_timeout_steps = 100u;
}

double oc_l2_width_us(const OcL2Params *p, double fuel_pct) {
    return p->injector_dead_us + p->injector_full_us * clamp(fuel_pct, 0.0, 100.0) / 100.0;
}

double oc_l2_fuel_pct(const OcL2Params *p, double width_us) {
    if (!(p->injector_full_us > 0.0)) {
        return 0.0;
    }
    return clamp(100.0 * (width_us - p->injector_dead_us) / p->injector_full_us, 0.0, 100.0);
}

void oc_l2_init(OcL2Plant *s, const PlantParams *pp) {
    memset(s, 0, sizeof(*s));
    plant_init(&s->plant, pp);
    for (unsigned c = 0; c < OC_L2_CYLINDERS; c++) {
        s->cyl_counted[c] = -1;
    }
}

double oc_l2_fuel_mean(const OcL2Plant *s) {
    double sum = 0.0;
    for (unsigned c = 0; c < OC_L2_CYLINDERS; c++) {
        sum += s->cyl_fuel_pct[c];
    }
    return sum / (double)OC_L2_CYLINDERS;
}

/* The unwrapped angle, in degrees since step 0, at a step's start. */
static double record_deg(const OcL2StepRecord *r) {
    return (double)r->cycle * CYCLE_DEG + oc_l2_acc_deg(r->acc);
}

static uint32_t delay_steps(const OcL2Params *p) {
    return (p->capture_delay_steps < OC_L2_HISTORY - 1u) ? p->capture_delay_steps : OC_L2_HISTORY - 1u;
}

/* Every window that closed during step j: a misfire if its cylinder had no counted injection in it. */
static void check_windows(OcL2Plant *s, const OcL2Params *p, uint32_t j) {
    const OcL2StepRecord *r = &s->hist[j % OC_L2_HISTORY];
    const double from = record_deg(r);
    const double to = from + (double)r->inc * (double)OC_L2_STEP_TICKS / ACC_FULL * CYCLE_DEG;
    for (unsigned c = 0; c < OC_L2_CYLINDERS; c++) {
        const double close = TDC_DEG[c] + p->window_after_deg;
        double k = ceil((from - close) / CYCLE_DEG);
        if (k < 0.0) {
            k = 0.0;
        }
        for (; k * CYCLE_DEG + close < to; k += 1.0) {
            if (s->cyl_counted[c] != (int64_t)k) {
                s->cyl_fuel_pct[c] = 0.0;
                s->misfires++;
            }
        }
    }
}

uint32_t oc_l2_step(OcL2Plant *s, const PlantParams *pp, const OcL2Params *p, const OcL2Inputs *in) {
    uint32_t inc = 0u;
    if (!otk_bridge_phase_inc(s->plant.rpm, OC_L2_TICK_HZ, &inc)) {
        inc = 0u;
        s->bad_speed++;
    }
    OcL2StepRecord *r = &s->hist[s->step % OC_L2_HISTORY];
    r->cycle = s->cycle;
    r->acc = s->acc;
    r->inc = inc;

    const PlantInputs pin = {
        .fuel_pct = oc_l2_fuel_mean(s),
        .load_torque_nm = in->load_torque_nm,
        .starter_engaged = in->starter_engaged,
    };
    plant_step(&s->plant, pp, &pin, OC_L2_STEP_S);

    const uint64_t phase = s->acc + (uint64_t)inc * OC_L2_STEP_TICKS;
    s->cycle += phase >> ACC_BITS;
    s->acc = phase & ACC_MASK;
    s->inc = inc;
    s->step++;

    /* A window is judged once its injection has had time to arrive. */
    const uint32_t d = delay_steps(p);
    while (s->checked_through + d < s->step) {
        check_windows(s, p, s->checked_through);
        s->checked_through++;
    }
    return inc;
}

OcL2CaptureResult oc_l2_capture(OcL2Plant *s, const OcL2Params *p, unsigned channel, uint64_t start_tick,
                                uint32_t width_ticks, double *deg) {
    const uint64_t j64 = start_tick / OC_L2_STEP_TICKS;
    if (j64 >= s->step || s->step - j64 > OC_L2_HISTORY) {
        s->late_captures++;
        return OC_L2_CAPTURE_LATE;
    }
    const uint32_t j = (uint32_t)j64;
    const OcL2StepRecord *r = &s->hist[j % OC_L2_HISTORY];
    const uint64_t phase = r->acc + (uint64_t)r->inc * (start_tick - j64 * OC_L2_STEP_TICKS);
    const uint64_t cycle = r->cycle + (phase >> ACC_BITS);
    const double a = oc_l2_acc_deg(phase);
    if (deg != NULL) {
        *deg = a;
    }

    for (unsigned c = 0; c < OC_L2_CYLINDERS; c++) {
        double rel = a - TDC_DEG[c];
        if (rel >= 360.0) {
            rel -= CYCLE_DEG;
        } else if (rel < -360.0) {
            rel += CYCLE_DEG;
        }
        if (rel < -p->window_before_deg || rel > p->window_after_deg) {
            continue;
        }
        if (channel != c) {
            break; /* in cylinder c + 1's window, on another channel */
        }
        /* The window's cycle: the cycle of its TDC, which may be the next one
         * (cylinder 1's window begins 40 degrees before 0) or the previous. */
        int64_t k = (int64_t)cycle;
        if (a - rel >= CYCLE_DEG) {
            k += 1;
        } else if (a - rel < 0.0) {
            k -= 1;
        }
        s->cyl_fuel_pct[c] = oc_l2_fuel_pct(p, (double)width_ticks * 1e6 / (double)OC_L2_TICK_HZ);
        s->cyl_counted[c] = k;
        s->injections++;
        return OC_L2_CAPTURE_COUNTED;
    }
    s->misplaced++;
    return OC_L2_CAPTURE_MISPLACED;
}

bool oc_l2_angle_at(const OcL2Plant *s, uint64_t tick, double *deg) {
    const uint64_t j64 = tick / OC_L2_STEP_TICKS;
    if (j64 >= s->step || s->step - j64 > OC_L2_HISTORY) {
        return false;
    }
    const OcL2StepRecord *r = &s->hist[j64 % OC_L2_HISTORY];
    const uint64_t phase = r->acc + (uint64_t)r->inc * (tick - j64 * OC_L2_STEP_TICKS);
    *deg = (double)(r->cycle + (phase >> ACC_BITS)) * CYCLE_DEG + oc_l2_acc_deg(phase);
    return true;
}
