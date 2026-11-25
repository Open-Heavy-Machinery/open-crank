/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file machine_ctrl.c
 * @brief Reference Level 1 machine controller. See machine_ctrl.h.
 */

#include <opencrank/machine_ctrl.h>

const McParams MC_DEFAULT_PARAMS = {
    .source_address = OC_ADDR_MACHINE_CTRL,
    .engine_address = J1939_ADDR_ENGINE_1,
    .eec1_timeout_ms = 200U,
    .operator_timeout_ms = 200U,
    .run_confirm_ms = 500U,
    .override_confirm_ms = 500U,
    .running_rpm = 600.0f,
    .stall_rpm = 400.0f,
    .overspeed_rpm = 2300.0f,
    .request_min_rpm = 750.0f,
    .request_max_rpm = 2100.0f,
    .slew_rpm_per_s = 500.0f,
};

static float clampf(float v, float lo, float hi) {
    float r = v;
    if (!(v >= lo)) { /* NaN maps to lo */
        r = lo;
    } else if (v > hi) {
        r = hi;
    } else {
        /* within range */
    }
    return r;
}

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) {
    return (uint32_t)(now_ms - since_ms) >= span_ms; /* wrap-safe */
}

void mc_init(McCtx *c, const McParams *p) {
    McCtx zero = { 0 };
    *c = zero;
    c->p = p;
    c->state = MC_STATE_WAIT_ENGINE;
    c->fault = MC_FAULT_NONE;
    c->controlling_address = J1939_ADDR_GLOBAL;
    c->target_rpm = p->request_min_rpm;
    c->requested_rpm = p->request_min_rpm;
}

void mc_handle_frame(McCtx *c, const J1939Frame *f, uint32_t now_ms) {
    const uint8_t src = j1939_id_source(f->id);
    J1939Eec1 eec1;
    OcOperator op;

    if ((src == c->p->engine_address) && j1939_decode_eec1(f, &eec1)) {
        if (eec1.engine_speed_valid) { /* status without a valid speed is no status */
            c->eec1_seen = true;
            c->eec1_ms = now_ms;
            c->engine_rpm = eec1.engine_speed_rpm;
            c->controlling_address = eec1.controlling_address;
        }
    } else if (oc_decode_operator(f, &op)) {
        c->operator_seen = true;
        c->operator_ms = now_ms;
        c->op = op;
        if (op.fault_reset_pressed && !c->reset_was_pressed) {
            c->reset_requested = true; /* act on the press, not on holding it */
        }
        c->reset_was_pressed = op.fault_reset_pressed;
    } else {
        /* not for us */
    }
}

void mc_clear_fault(McCtx *c) {
    c->reset_requested = true;
}

static void enter_fault(McCtx *c, McFault fault) {
    c->state = MC_STATE_FAULT;
    c->fault = fault;
}

static void enter_run(McCtx *c) {
    c->state = MC_STATE_RUN;
    c->requested_rpm = clampf(c->engine_rpm, c->p->request_min_rpm, c->p->request_max_rpm);
    c->override_mismatch = false;
}

/* While RUN: the engine must show this controller in control (SPN 1483). */
static bool override_rejected(McCtx *c, uint32_t now_ms) {
    if (c->controlling_address == c->p->source_address) {
        c->override_mismatch = false;
        return false; /* DEV-004: in control, nothing to time */
    }
    if (!c->override_mismatch) {
        c->override_mismatch = true;
        c->override_mismatch_since_ms = now_ms;
        return false; /* DEV-004: mismatch starts, timing begins */
    }
    return elapsed(now_ms, c->override_mismatch_since_ms, c->p->override_confirm_ms);
}

size_t mc_step(McCtx *c, uint32_t now_ms, J1939Frame *out, size_t cap) {
    const McParams *p = c->p;
    const float dt_s = c->stepped ? ((float)(uint32_t)(now_ms - c->last_step_ms) / 1000.0f) : 0.0f;
    c->stepped = true;
    c->last_step_ms = now_ms;

    const bool status_fresh = c->eec1_seen && !elapsed(now_ms, c->eec1_ms, p->eec1_timeout_ms + 1U);
    const bool operator_fresh = c->operator_seen && !elapsed(now_ms, c->operator_ms, p->operator_timeout_ms + 1U);

    c->target_rpm = (operator_fresh && c->op.target_speed_valid)
                        ? clampf(c->op.target_speed_rpm, p->request_min_rpm, p->request_max_rpm)
                        : p->request_min_rpm;
    const float hoist_pct =
        (operator_fresh && c->op.hoist_demand_valid) ? clampf(c->op.hoist_demand_pct, 0.0f, 100.0f) : 0.0f;
    const bool reset = c->reset_requested;
    c->reset_requested = false;

    switch (c->state) {
    case MC_STATE_WAIT_ENGINE:
        if (status_fresh && (c->engine_rpm >= p->running_rpm)) {
            if (!c->running_seen) {
                c->running_seen = true;
                c->running_since_ms = now_ms;
            } else if (elapsed(now_ms, c->running_since_ms, p->run_confirm_ms)) {
                enter_run(c);
            } else {
                /* keep confirming */
            }
        } else {
            c->running_seen = false;
        }
        break;

    case MC_STATE_RUN:
        if (!status_fresh) {
            enter_fault(c, MC_FAULT_ENGINE_STATUS_LOST);
        } else if (c->engine_rpm > p->overspeed_rpm) {
            enter_fault(c, MC_FAULT_OVERSPEED);
        } else if (c->engine_rpm < p->stall_rpm) {
            enter_fault(c, MC_FAULT_STALL);
        } else if (override_rejected(c, now_ms)) {
            enter_fault(c, MC_FAULT_OVERRIDE_REJECTED);
        } else {
            const float max_delta = p->slew_rpm_per_s * dt_s;
            c->requested_rpm += clampf(c->target_rpm - c->requested_rpm, -max_delta, max_delta);
        }
        break;

    case MC_STATE_FAULT:
    default:
        if (reset) {
            c->state = MC_STATE_WAIT_ENGINE;
            c->fault = MC_FAULT_NONE;
            c->running_seen = false;
        }
        break;
    }

    const bool run = (c->state == MC_STATE_RUN);
    c->pump_cmd_pct = run ? hoist_pct : 0.0f;

    size_t n = 0U;
    if (n < cap) {
        J1939Tsc1 tsc1 = {
            .override_mode = run ? J1939_OVERRIDE_SPEED : J1939_OVERRIDE_DISABLED,
            .speed_conditions = 0U,
            .mode_priority = 3U,
            .requested_speed_valid = run,
            .requested_speed_rpm = c->requested_rpm,
            .requested_torque_valid = false,
            .requested_torque_pct = 0.0f,
            .message_counter = c->tsc1_counter,
            .checksum_valid = false, /* decode only */
        };
        j1939_encode_tsc1(&tsc1, p->source_address, p->engine_address, &out[n]);
        c->tsc1_counter = (uint8_t)((uint8_t)(c->tsc1_counter + 1U) & 0x0FU); /* SPN 4206: 15 wraps to 0 */
        n++;
    }
    if (n < cap) {
        OcPumpCmd pump = { .displacement_valid = true, .displacement_pct = c->pump_cmd_pct };
        oc_encode_pump_cmd(&pump, p->source_address, &out[n]);
        n++;
    }
    return n;
}

const char *mc_state_name(McState s) {
    const char *name = "?";
    switch (s) {
    case MC_STATE_WAIT_ENGINE:
        name = "WAIT_ENGINE";
        break;
    case MC_STATE_RUN:
        name = "RUN";
        break;
    case MC_STATE_FAULT:
        name = "FAULT";
        break;
    default:
        break;
    }
    return name;
}

const char *mc_fault_name(McFault f) {
    const char *name = "?";
    switch (f) {
    case MC_FAULT_NONE:
        name = "none";
        break;
    case MC_FAULT_ENGINE_STATUS_LOST:
        name = "engine status lost";
        break;
    case MC_FAULT_OVERSPEED:
        name = "overspeed";
        break;
    case MC_FAULT_STALL:
        name = "stall";
        break;
    case MC_FAULT_OVERRIDE_REJECTED:
        name = "override rejected";
        break;
    default:
        break;
    }
    return name;
}
