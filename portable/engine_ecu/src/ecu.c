/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file ecu.c
 * @brief Reference engine-ECU logic for Level 2. See ecu.h.
 */

#include <opencrank/ecu.h>

const EcuParams ECU_DEFAULT_PARAMS = {
    .tick_hz = 170000000U,
    .tooth0_deg = -60.0f,
    .cam_deg = 90.0f,
    .cam_window_deg = 30.0f,
    .tdc_deg = { 0.0f, 480.0f, 240.0f, 600.0f, 120.0f, 360.0f },
    .soi_btdc_deg = 10.0f,
    .injector_dead_us = 200.0f,
    .injector_full_us = 1600.0f,
    .no_tooth_ms = 100U,
    .low_idle_rpm = 750.0f,
    .high_idle_rpm = 2100.0f,
    .overspeed_trip_rpm = 2400.0f,
    .running_rpm = 500.0f,
    .start_fuel_pct = 30.0f,
    .kp_pct_per_rpm = 0.10f,
    .ki_pct_per_rpm_s = 0.30f,
    .request_timeout_ms = 200U,
    .source_address = 0x00U,
};

#define PITCH_DEG 6.0f   /* 360 / 60 */
#define REV_DEG 360.0f
#define CYCLE_DEG 720.0f

enum {
    SPEED_POSITIONS = 20, /* 120 degrees */
    GAP_POSITIONS = 3     /* the gap spans three pitches */
};

static float ecu_clampf(float v, float lo, float hi) {
    float r = v;
    if (!(v >= lo)) { /* also catches NaN */
        r = lo;
    } else if (v > hi) {
        r = hi;
    } else {
        /* within range */
    }
    return r;
}

/* v modulo range, in [0, range), for |v| well below 2^31 * range. */
static float wrapf(float v, float range) {
    const float q = v / range;
    int32_t k = (int32_t)q;
    if ((float)k > q) {
        k -= 1;
    }
    const float whole = (float)k * range;
    float r = v - whole;
    if (r < 0.0f) {
        r += range;
    } else if (r >= range) {
        r -= range;
    } else {
        /* in range */
    }
    return r;
}

/* ---- the wheel ------------------------------------------------------- */

/* Where cylinder c's injection is armed: the reference tooth is the last real
 * tooth at least one pitch before the start of injection; it is armed on the
 * edge before that. */
static EcuArmPoint arm_point(const EcuParams *p, unsigned c) {
    const float soi = wrapf(p->tdc_deg[c] - p->soi_btdc_deg - p->tooth0_deg, CYCLE_DEG);
    uint32_t rev = (soi >= REV_DEG) ? 1U : 0U;
    float in_rev = soi - ((float)rev * REV_DEG);
    float ref_f = (in_rev - PITCH_DEG) / PITCH_DEG;
    if (ref_f < 0.0f) { /* the reference is in the revolution before */
        rev ^= 1U;
        in_rev += REV_DEG;
        ref_f = (in_rev - PITCH_DEG) / PITCH_DEG;
    }
    uint32_t ref = (uint32_t)ref_f;
    if (ref >= (uint32_t)ECU_TEETH) {
        ref = (uint32_t)ECU_TEETH - 1U; /* never a tooth of the gap */
    }
    EcuArmPoint a;
    a.delay_deg = in_rev - ((float)ref * PITCH_DEG);
    if (ref == 0U) {
        a.arm_tooth = (uint8_t)(ECU_TEETH - 1);
        a.arm_rev = (uint8_t)(rev ^ 1U);
    } else {
        a.arm_tooth = (uint8_t)(ref - 1U);
        a.arm_rev = (uint8_t)rev;
    }
    return a;
}

void ecu_wheel_init(EcuWheel *w, const EcuParams *p) {
    /* in flash: clears the context without a large object on the stack
     * (ecu_wheel_poll() runs in an interrupt) */
    static const EcuWheel wheel_zero = { 0 };
    *w = wheel_zero;
    w->p = p;
    for (unsigned c = 0U; c < (unsigned)ECU_CYLINDERS; c++) {
        w->arm[c] = arm_point(p, c);
    }
}

static void lose_sync(EcuWheel *w) {
    if (w->st.sync >= ECU_SYNC_CRANK) {
        w->st.sync_losses++;
    }
    w->st.sync = ECU_SYNC_SEEKING;
    w->counting = false;
    w->gap_pending = false;
    w->cam_in_window = false;
    w->cam_wrong = false;
    w->cam_errors = 0U;
}

/* The end of a 720-degree cycle in FULL: was the cam where it belongs? */
static void end_of_cycle(EcuWheel *w) {
    if (w->cam_in_window && !w->cam_wrong) {
        w->cam_errors = 0U;
    } else if (w->cam_errors < 255U) {
        w->cam_errors++;
    } else {
        /* saturated */
    }
    w->cam_in_window = false;
    w->cam_wrong = false;
}

/* A new revolution begins at tooth 0 (the edge after the gap). */
static void new_revolution(EcuWheel *w) {
    if (w->st.sync == ECU_SYNC_FULL) {
        if (w->st.rev == 1U) {
            end_of_cycle(w);
        }
        w->st.rev ^= 1U;
        if (w->cam_errors >= 2U) {
            lose_sync(w);
        }
    }
}

/* A gap confirmed by the edge after it: the edge before was tooth 0, this one is tooth 1. */
static void gap_confirmed(EcuWheel *w, uint8_t idx_of_gap_edge) {
    w->st.gaps++;
    if (w->st.sync >= ECU_SYNC_CRANK) {
        /* expected: tooth 0 was already counted as the new revolution */
    } else if (w->counting && (idx_of_gap_edge == (uint8_t)ECU_TEETH)) {
        if (w->cam_in_window) {
            /* the revolution just counted had the cam: it was revolution 0 */
            w->st.sync = ECU_SYNC_FULL;
            w->st.rev = 1U;
            w->cam_wrong = false;
            w->cam_errors = 0U;
        } else {
            w->st.sync = ECU_SYNC_CRANK;
        }
    } else {
        /* the first gap, or the count was wrong: count again from here */
    }
    if (w->st.sync < ECU_SYNC_CRANK) {
        w->cam_in_window = false; /* a new revolution while seeking */
    }
    w->counting = true;
    w->idx = 1U;
}

static void speed_update(EcuWheel *w, uint32_t t, uint32_t positions) {
    w->pos += positions;
    w->ring_t[w->ring_head] = t;
    w->ring_pos[w->ring_head] = w->pos;
    w->ring_head = (uint8_t)(((uint32_t)w->ring_head + 1U) % (uint32_t)ECU_SPEED_RING);
    if (w->ring_n < (uint8_t)ECU_SPEED_RING) {
        w->ring_n++;
    }
    /* the newest edge at least SPEED_POSITIONS back, or the oldest kept */
    uint32_t span_pos = 0U;
    uint32_t span_t = 0U;
    for (uint32_t k = 1U; k < (uint32_t)w->ring_n; k++) {
        const uint32_t i = ((uint32_t)w->ring_head + (uint32_t)ECU_SPEED_RING - 1U - k) % (uint32_t)ECU_SPEED_RING;
        span_pos = w->pos - w->ring_pos[i];
        span_t = t - w->ring_t[i];
        if (span_pos >= (uint32_t)SPEED_POSITIONS) {
            break;
        }
    }
    if ((span_pos != 0U) && (span_t != 0U)) {
        w->st.rpm = ((float)span_pos * (float)w->p->tick_hz) / (float)span_t;
    }
}

/* The cylinder to arm on edge (idx, rev), if any. */
static bool arm_for(const EcuWheel *w, uint8_t idx, uint8_t rev, EcuArm *arm) {
    bool found = false;
    for (unsigned c = 0U; (c < (unsigned)ECU_CYLINDERS) && !found; c++) {
        const EcuArmPoint *a = &w->arm[c];
        if ((a->arm_tooth == idx) && (a->arm_rev == rev)) {
            arm->injector = (uint8_t)(c + 1U);
            const float delay = (a->delay_deg / PITCH_DEG) * (float)w->last_dt;
            arm->delay_ticks = (uint32_t)delay;
            arm->width_ticks = w->width_ticks;
            found = true;
        }
    }
    return found;
}

bool ecu_wheel_tooth(EcuWheel *w, uint32_t t, EcuArm *arm) {
    bool armed = false;
    w->st.teeth++;
    if (!w->have_edge) {
        w->have_edge = true;
        w->last_t = t;
        w->st.sync = ECU_SYNC_SEEKING;
        speed_update(w, t, 0U);
    } else {
        const uint32_t dt = t - w->last_t; /* wrap-safe */
        const bool long_period = (w->last_dt != 0U) && (((uint64_t)dt * 2U) > ((uint64_t)w->last_dt * 3U));
        bool positions_gap = false;
        if (w->gap_pending) {
            w->gap_pending = false;
            if (((uint64_t)dt * 2U) < (uint64_t)w->gap_dt) {
                gap_confirmed(w, w->idx);
            } else {
                lose_sync(w); /* a long period that was not the gap */
            }
        } else if (long_period) {
            /* this edge is tooth 0 if the next period confirms it */
            w->gap_pending = true;
            w->gap_dt = dt;
            positions_gap = true;
            if (w->counting) {
                w->idx++;
            }
            if ((w->st.sync >= ECU_SYNC_CRANK) && (w->idx != (uint8_t)ECU_TEETH)) {
                lose_sync(w); /* a gap where none belongs */
            }
        } else if (w->counting) {
            w->idx++;
            if ((w->st.sync >= ECU_SYNC_CRANK) && (w->idx >= (uint8_t)ECU_TEETH)) {
                lose_sync(w); /* no gap where one belongs */
            }
        } else {
            /* seeking the first gap */
        }

        if (w->counting && (w->idx == (uint8_t)ECU_TEETH) && (w->st.sync >= ECU_SYNC_CRANK)) {
            w->idx = 0U; /* tooth 0 */
            new_revolution(w);
        }
        speed_update(w, t, positions_gap ? (uint32_t)GAP_POSITIONS : 1U);
        if (!positions_gap) {
            w->last_dt = dt; /* the gap's period is not a tooth period */
        }
        w->last_t = t;

        if ((w->st.sync == ECU_SYNC_FULL) && (w->width_ticks != 0U)) {
            armed = arm_for(w, w->idx, (uint8_t)w->st.rev, arm);
            if (armed) {
                w->st.arms++;
            }
        }
    }
    w->st.tooth = w->counting ? w->idx : 0U;
    return armed;
}

void ecu_wheel_cam(EcuWheel *w, uint32_t t) {
    w->st.cam_edges++;
    if (w->counting && (w->last_dt != 0U)) {
        /* the cam's angle in this revolution, from the last tooth and the time since */
        const uint32_t since = t - w->last_t;
        const float frac = (float)since / (float)w->last_dt;
        const float angle = w->p->tooth0_deg + (((float)w->idx + frac) * PITCH_DEG);
        const float off = wrapf(angle - w->p->cam_deg + 180.0f, REV_DEG) - 180.0f;
        const bool in_window = (off >= -w->p->cam_window_deg) && (off <= w->p->cam_window_deg);
        if (w->st.sync == ECU_SYNC_SEEKING) {
            /* counting teeth after a first gap: remember the cam for the
             * second gap, which then gives FULL at once */
            if (in_window) {
                w->cam_in_window = true;
            }
        } else if (w->st.sync == ECU_SYNC_CRANK) {
            if (in_window) {
                w->st.sync = ECU_SYNC_FULL;
                w->st.rev = 0U;
                w->cam_in_window = true;
                w->cam_wrong = false;
                w->cam_errors = 0U;
            }
        } else if (in_window && (w->st.rev == 0U) && !w->cam_in_window) {
            w->cam_in_window = true;
        } else {
            w->cam_wrong = true;
        }
    }
}

void ecu_wheel_poll(EcuWheel *w, uint32_t now) {
    const uint32_t limit = (w->p->tick_hz / 1000U) * w->p->no_tooth_ms;
    if (w->have_edge && ((now - w->last_t) > limit)) {
        const uint32_t teeth = w->st.teeth;
        const uint32_t cams = w->st.cam_edges;
        const uint32_t gaps = w->st.gaps;
        const uint32_t losses = w->st.sync_losses;
        const uint32_t stops = w->st.stops;
        const uint32_t arms = w->st.arms;
        const uint32_t width = w->width_ticks;
        ecu_wheel_init(w, w->p);
        w->st.teeth = teeth;
        w->st.cam_edges = cams;
        w->st.gaps = gaps;
        w->st.sync_losses = losses;
        w->st.stops = stops + 1U;
        w->st.arms = arms;
        w->width_ticks = width;
    }
}

void ecu_wheel_set_width(EcuWheel *w, uint32_t width_ticks) {
    w->width_ticks = width_ticks;
}

void ecu_wheel_status(const EcuWheel *w, EcuWheelStatus *out) {
    *out = w->st;
}

/* ---- the governor ---------------------------------------------------- */

void ecu_init(EcuCtx *e, const EcuParams *p) {
    static const EcuCtx ctx_zero = { 0 };
    *e = ctx_zero;
    e->p = p;
    e->state = ECU_GOV_STOPPED;
    e->target_rpm = p->low_idle_rpm;
    e->controlling_address = p->source_address;
}

void ecu_handle_frame(EcuCtx *e, const J1939Frame *f, uint32_t now_ms) {
    J1939Tsc1 m;
    const uint8_t dest = j1939_id_dest(f->id);
    if (((dest == e->p->source_address) || (dest == J1939_ADDR_GLOBAL)) && j1939_decode_tsc1(f, &m)) {
        const bool repeated = e->counter_seen && (m.message_counter == e->counter);
        if (!m.checksum_valid || repeated) {
            e->tsc1_rejected++;
        } else {
            e->counter_seen = true;
            e->counter = m.message_counter;
            e->request_seen = true;
            e->request_ms = now_ms;
            e->request_speed = (m.override_mode == J1939_OVERRIDE_SPEED) && m.requested_speed_valid;
            e->request_rpm = m.requested_speed_rpm;
            e->request_address = j1939_id_source(f->id);
        }
    }
}

static void select_setpoint(EcuCtx *e, uint32_t now_ms) {
    const EcuParams *p = e->p;
    const bool fresh = e->request_seen && ((now_ms - e->request_ms) <= p->request_timeout_ms);
    if (fresh && e->request_speed) {
        e->from_request = true;
        e->target_rpm = ecu_clampf(e->request_rpm, p->low_idle_rpm, p->high_idle_rpm);
        e->controlling_address = e->request_address;
    } else {
        e->from_request = false;
        e->target_rpm = p->low_idle_rpm;
        e->controlling_address = p->source_address;
    }
}

/* Level 1's PI with conditional-integration anti-windup (sim/engine_ecu), isochronous. */
static float governor(EcuCtx *e, float rpm, float dt) {
    const EcuParams *p = e->p;
    const float error = e->target_rpm - rpm;
    const float proportional = p->kp_pct_per_rpm * error;
    const float unclamped = proportional + e->integrator_pct;
    const bool up_into_limit = (unclamped >= 100.0f) && (error > 0.0f);
    const bool down_into_limit = (unclamped <= 0.0f) && (error < 0.0f);
    if (!up_into_limit && !down_into_limit) {
        e->integrator_pct = ecu_clampf(e->integrator_pct + (p->ki_pct_per_rpm_s * error * dt), 0.0f, 100.0f);
    }
    return ecu_clampf(proportional + e->integrator_pct, 0.0f, 100.0f);
}

static uint32_t width_for(const EcuParams *p, float fuel_pct) {
    uint32_t ticks = 0U;
    if (fuel_pct > 0.0f) {
        const float us = p->injector_dead_us + ((p->injector_full_us * fuel_pct) / 100.0f);
        const float t = us * ((float)p->tick_hz / 1000000.0f);
        ticks = (uint32_t)t;
    }
    return ticks;
}

size_t ecu_step(EcuCtx *e, const EcuWheelStatus *ws, uint32_t now_ms, J1939Frame *out, size_t cap) {
    const EcuParams *p = e->p;
    const uint32_t dt_ms = e->stepped ? (now_ms - e->last_step_ms) : 0U;
    const float dt = (float)dt_ms / 1000.0f;
    e->stepped = true;
    e->last_step_ms = now_ms;
    const float rpm = ws->rpm;
    e->rpm = rpm;

    if (rpm > p->overspeed_trip_rpm) {
        e->overspeed_tripped = true;
    } else if (ws->sync == ECU_SYNC_NONE) {
        e->overspeed_tripped = false; /* the engine has stopped */
    } else {
        /* latched */
    }
    select_setpoint(e, now_ms);

    const bool phase_known = (ws->sync == ECU_SYNC_FULL);
    switch (e->state) {
    case ECU_GOV_STOPPED:
        if (phase_known && !e->overspeed_tripped) {
            e->state = (rpm >= p->running_rpm) ? ECU_GOV_RUNNING : ECU_GOV_CRANKING;
            e->integrator_pct = ecu_clampf(p->start_fuel_pct - (p->kp_pct_per_rpm * (e->target_rpm - rpm)), 0.0f, 100.0f);
        }
        break;
    case ECU_GOV_CRANKING:
        if (!phase_known) {
            e->state = ECU_GOV_STOPPED;
        } else if (rpm >= p->running_rpm) {
            /* bumpless hand-over: the governor starts from the start fuel */
            e->state = ECU_GOV_RUNNING;
            e->integrator_pct = ecu_clampf(p->start_fuel_pct - (p->kp_pct_per_rpm * (e->target_rpm - rpm)), 0.0f, 100.0f);
        } else {
            /* still cranking */
        }
        break;
    case ECU_GOV_RUNNING:
        if (!phase_known) {
            e->state = ECU_GOV_STOPPED; /* sync lost, or stopped */
        }
        break;
    default:
        e->state = ECU_GOV_STOPPED;
        break;
    }

    float fuel = 0.0f;
    if (e->state == ECU_GOV_CRANKING) {
        fuel = p->start_fuel_pct;
    } else if (e->state == ECU_GOV_RUNNING) {
        fuel = governor(e, rpm, dt);
    } else {
        e->integrator_pct = 0.0f;
    }
    if (e->overspeed_tripped) {
        fuel = 0.0f;
    }
    e->fuel_pct = fuel;
    e->width_ticks = width_for(p, fuel);

    size_t n = 0U;
    if (cap >= 1U) {
        const J1939Eec1 m = {
            .torque_mode = e->from_request ? J1939_TORQUE_MODE_NA : J1939_TORQUE_MODE_LOW_IDLE,
            .driver_demand_torque_valid = true,
            .driver_demand_torque_pct = fuel,
            .actual_torque_valid = true,
            .actual_torque_pct = fuel,
            .engine_speed_valid = true,
            .engine_speed_rpm = rpm,
            .controlling_address = e->controlling_address,
        };
        j1939_encode_eec1(&m, p->source_address, &out[0]);
        n = 1U;
    }
    return n;
}

uint32_t ecu_width_ticks(const EcuCtx *e) {
    return e->width_ticks;
}
