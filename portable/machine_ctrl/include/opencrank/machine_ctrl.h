/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file machine_ctrl.h
 * @brief Reference Level 1 machine controller. Portable: no I/O, no clock,
 *        no allocation, float only. This is the code meant for the STM32.
 *
 * The platform feeds it every received frame and calls mc_step() every
 * period; it sends whatever frames mc_step() returns. Time is a millisecond
 * counter supplied by the caller (wrap-safe).
 *
 * States:
 *
 *   WAIT_ENGINE --(fresh EEC1, speed >= running for run_confirm)--> RUN
 *   RUN --(EEC1 timeout | overspeed | stall | override not honoured)--> FAULT
 *   FAULT --(fault-reset button or mc_clear_fault())--> WAIT_ENGINE
 *
 * In RUN it asks the engine for the operator's target speed through TSC1
 * (speed control, slew-limited) and passes the hoist lever to the pump.
 * Outside RUN it releases the override (TSC1 override disabled, so the
 * engine returns to its own idle governor) and commands zero displacement.
 */

#ifndef OPEN_CRANK_MACHINE_CTRL_H
#define OPEN_CRANK_MACHINE_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/j1939.h>
#include <opencrank/j1939_diag.h>
#include <opencrank/oc_msgs.h>

typedef enum {
    MC_STATE_WAIT_ENGINE = 0,
    MC_STATE_RUN,
    MC_STATE_FAULT
} McState;

typedef enum {
    MC_FAULT_NONE = 0,
    MC_FAULT_ENGINE_STATUS_LOST,  /**< no valid EEC1 within eec1_timeout_ms */
    MC_FAULT_OVERSPEED,           /**< engine speed above overspeed_rpm */
    MC_FAULT_STALL,               /**< engine speed fell below stall_rpm while in RUN */
    MC_FAULT_OVERRIDE_REJECTED,   /**< EEC1 SPN 1483 does not show this controller in control */
    MC_FAULT_WATCHDOG_RESET,      /**< the platform restarted after its watchdog fired (mc_init_after_watchdog) */
    MC_FAULT_ENGINE_RED_STOP      /**< the engine's DM1 shows its red stop lamp */
} McFault;

/** @brief Controller configuration. Provenance: docs/parameters.md. */
typedef struct {
    uint8_t source_address;
    uint8_t engine_address;
    uint32_t eec1_timeout_ms;
    uint32_t operator_timeout_ms;
    uint32_t run_confirm_ms;
    uint32_t override_confirm_ms;
    float running_rpm;
    float stall_rpm;
    float overspeed_rpm;
    float request_min_rpm;
    float request_max_rpm;
    float slew_rpm_per_s;
    float limp_rpm;                 /**< limp-home: requested speed at most this while the engine's amber lamp is on */
    float limp_pump_pct;            /**< limp-home: pump command at most this */
    float limp_pump_rate_pct_per_s; /**< how fast the pump limit moves in and out of limp-home */
} McParams;

extern const McParams MC_DEFAULT_PARAMS;

/** @brief Controller state. Read-only for callers. */
typedef struct {
    const McParams *p;
    McState state;
    McFault fault;

    bool stepped;
    uint32_t last_step_ms;

    bool eec1_seen;
    uint32_t eec1_ms;
    float engine_rpm;
    uint8_t controlling_address;

    bool operator_seen;
    uint32_t operator_ms;
    OcOperator op;
    bool reset_was_pressed;
    bool reset_requested;

    bool running_seen;
    uint32_t running_since_ms;
    bool override_mismatch;
    uint32_t override_mismatch_since_ms;

    float target_rpm;     /**< operator target after clamping */
    float requested_rpm;  /**< slew-limited request sent in TSC1 */
    float pump_cmd_pct;   /**< displacement command sent to the pump */
    uint8_t tsc1_counter; /**< message counter of the next TSC1 (SPN 4206), 0..15 */

    J1939BamRx engine_bam;  /**< the engine's multi-packet DM1 (BAM) */
    bool engine_red_stop;   /**< the engine's last DM1 shows the red stop lamp */
    bool engine_amber;      /**< the engine's last DM1 shows the amber warning lamp: limp-home */
    float pump_limit_pct;   /**< pump command limit: 100, or moving towards limp_pump_pct */
} McCtx;

void mc_init(McCtx *c, const McParams *p);

/**
 * @brief As mc_init(), but for a restart caused by the platform's watchdog:
 *        the controller starts in FAULT (MC_FAULT_WATCHDOG_RESET) instead of
 *        WAIT_ENGINE, so it sends the safe state (TSC1 mode 00, pump at 0)
 *        and waits for the operator's fault reset (docs/safety.md, SR-7 and
 *        SR-11). A fault reset button already held when the controller
 *        starts does not count: it must be released and pressed again.
 *        The platform decides when to call it (for example, on IWDGRSTF).
 */
void mc_init_after_watchdog(McCtx *c, const McParams *p);

/** @brief Feeds one received frame. Frames the controller does not use are ignored. */
void mc_handle_frame(McCtx *c, const J1939Frame *f, uint32_t now_ms);

/**
 * @brief Runs one control period.
 * @return number of frames written to @p out (TSC1 and OC_PUMP_CMD), at most @p cap.
 */
size_t mc_step(McCtx *c, uint32_t now_ms, J1939Frame *out, size_t cap);

/** @brief Explicit fault clear (same effect as the operator's fault-reset button). */
void mc_clear_fault(McCtx *c);

const char *mc_state_name(McState s);
const char *mc_fault_name(McFault f);

#endif /* OPEN_CRANK_MACHINE_CTRL_H */
