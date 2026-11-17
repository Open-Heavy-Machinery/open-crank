/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file engine_ecu.c
 * @brief Simulated engine ECU. See engine_ecu.h.
 */

#include <opencrank/engine_ecu.h>

#include <string.h>

/** Below this speed the crankshaft is considered stopped. */
#define ECU_STOPPED_RPM 1.0

static double clamp(double v, double lo, double hi) {
    if (!(v >= lo)) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

void engine_ecu_init(EngineEcu *e, const EngineEcuParams *p) {
    memset(e, 0, sizeof(*e));
    e->state = ECU_STATE_OFF;
    e->source = ECU_SPEED_SOURCE_LOW_IDLE;
    e->target_rpm = p->low_idle_rpm;
    e->controlling_address = p->source_address;
}

void engine_ecu_request(EngineEcu *e, const EngineEcuRequest *req) {
    e->request = *req;
    e->request_seen = true;
    e->request_t_ms = e->t_ms;
}

/* Selects the governor setpoint: a fresh external speed request, or low idle. */
static void select_setpoint(EngineEcu *e, const EngineEcuParams *p) {
    const uint32_t age_ms = e->t_ms - e->request_t_ms; /* wrap-safe */
    const bool fresh = e->request_seen && !e->ignore_requests && age_ms <= p->request_timeout_ms;

    if (fresh && e->request.mode == ECU_OVERRIDE_SPEED && e->request.speed_valid) {
        e->source = ECU_SPEED_SOURCE_REQUEST;
        e->target_rpm = clamp(e->request.speed_rpm, p->low_idle_rpm, p->high_idle_rpm);
        e->controlling_address = e->request.source_address;
    } else {
        e->source = ECU_SPEED_SOURCE_LOW_IDLE;
        e->target_rpm = p->low_idle_rpm;
        e->controlling_address = p->source_address;
    }
}

/* PI speed governor with droop and conditional-integration anti-windup. */
static double governor(EngineEcu *e, const EngineEcuParams *p, double rpm, double dt) {
    const double setpoint = e->target_rpm * (1.0 - (p->droop_pct / 100.0) * (e->fuel_pct / 100.0));
    const double error = setpoint - rpm;
    const double proportional = p->kp_pct_per_rpm * error;
    const double unclamped = proportional + e->integrator_pct;

    const bool pushing_up_into_limit = (unclamped >= 100.0) && (error > 0.0);
    const bool pushing_down_into_limit = (unclamped <= 0.0) && (error < 0.0);
    if (!pushing_up_into_limit && !pushing_down_into_limit) {
        e->integrator_pct = clamp(e->integrator_pct + p->ki_pct_per_rpm_s * error * dt, 0.0, 100.0);
    }
    return clamp(proportional + e->integrator_pct, 0.0, 100.0);
}

void engine_ecu_step(EngineEcu *e, const EngineEcuParams *p, const EngineEcuInputs *in, uint32_t dt_ms,
                     EngineEcuOutputs *out) {
    const double dt = (double)dt_ms / 1000.0;
    const double rpm = in->rpm;
    e->t_ms += dt_ms;

    /* Overspeed: trip immediately, stay tripped until the shaft has stopped. */
    if (rpm > p->overspeed_trip_rpm) {
        e->overspeed_tripped = true;
    } else if (e->overspeed_tripped && rpm < ECU_STOPPED_RPM) {
        e->overspeed_tripped = false;
    }

    select_setpoint(e, p);

    if (!in->key_on) {
        e->state = ECU_STATE_OFF;
    } else {
        switch (e->state) {
        case ECU_STATE_OFF:
            e->state = ECU_STATE_STOPPED;
            break;
        case ECU_STATE_STOPPED:
            if (in->start && !e->overspeed_tripped && rpm < p->running_rpm) {
                e->state = ECU_STATE_CRANKING;
                e->crank_time_s = 0.0;
            }
            break;
        case ECU_STATE_CRANKING:
            e->crank_time_s += dt;
            if (rpm >= p->running_rpm) {
                /* Bumpless hand-over: the governor starts from the start fuel. */
                e->state = ECU_STATE_RUNNING;
                e->integrator_pct =
                    clamp(p->start_fuel_pct - p->kp_pct_per_rpm * (e->target_rpm - rpm), 0.0, 100.0);
            } else if (!in->start || e->crank_time_s > p->max_crank_s || e->overspeed_tripped) {
                e->state = ECU_STATE_STOPPED;
            }
            break;
        case ECU_STATE_RUNNING:
            if (rpm < p->stall_rpm) {
                e->state = ECU_STATE_STOPPED; /* stalled, or spun down after a trip */
            }
            break;
        default:
            e->state = ECU_STATE_STOPPED;
            break;
        }
    }

    double fuel = 0.0;
    switch (e->state) {
    case ECU_STATE_CRANKING:
        fuel = (rpm >= p->fuel_enable_rpm) ? p->start_fuel_pct : 0.0;
        break;
    case ECU_STATE_RUNNING:
        fuel = governor(e, p, rpm, dt);
        break;
    default:
        e->integrator_pct = 0.0;
        break;
    }
    if (e->overspeed_tripped) {
        fuel = 0.0;
    }

    e->fuel_pct = fuel;
    e->starter_engaged = (e->state == ECU_STATE_CRANKING);
    out->fuel_pct = fuel;
    out->starter_engaged = e->starter_engaged;
}
