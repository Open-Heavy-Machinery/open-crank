/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file l2_loop.h
 * @brief The Level 2 loop in memory (v0.4): the Level 2 plant, the hoist
 *        pump, the simulated machine controller and the operator panel on one
 *        side; the reference engine-ECU logic (portable/engine_ecu), as it runs
 *        on the board, on the other. Deterministic, no clock.
 *
 * One step is 1 ms, the bridge's step. In each step:
 *   1. the captures due now reach the plant (the DUT-to-plant delay);
 *   2. scenario events; the plant computes the step (oc_l2_step);
 *   3. the edge model turns the step's accumulator into the crank and cam
 *      edges the bridge would put out, in the bridge's ticks, converts them
 *      to the ECU's timer (170 MHz, with the HSI's bias) and calls the ECU's
 *      wheel in time order; an armed injection starts on the next crank edge
 *      the ECU sees, plus its delay, and becomes a capture on the bridge's
 *      tick grid; ecu_wheel_poll() once a step;
 *   4. every 10 ms the machine controller and the ECU's governor run, with
 *      TSC1 and EEC1 between them one step late, as in sim/runtime/loop.c;
 *      every 20 ms the operator panel; the pump follows its command with the
 *      bench's 200 ms timeout.
 *
 * The bridge's crank output follows its gateware (open-tick rotary.v): tooth i
 * of a revolution begins where the top 32 bits of the accumulator, less
 * crank_offset, reach ceil(i * 2^31 / 60); the cam where they reach cam_start.
 */

#ifndef OPEN_CRANK_L2_LOOP_H
#define OPEN_CRANK_L2_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/ecu.h>
#include <opencrank/l2/plant_l2.h>
#include <opencrank/loads/hydraulic_pump.h>
#include <opencrank/machine_ctrl.h>
#include <opencrank/runtime/scenario.h>
#include <opencrank/runtime/signals.h>
#include <opentick/can.h>
#include <opentick/player.h>

/** @brief The reference board's HSI against a host clock (F-22 in the project's notes; WIRING-L2.md). */
#define OC_L2_HSI_BIAS_DEFAULT (-2658e-6)

/** @brief Captures in flight to the plant, at most. */
#define OC_L2_CAPTURES 64U

/** @brief Configuration of the loop. */
typedef struct {
    const PlantParams *plant;
    const OcL2Params *l2;
    const HydraulicPumpParams *pump;
    const McParams *mc;
    const EcuParams *ecu;
    double hsi_bias;          /**< the ECU's clock, relative: -2658e-6 runs 0.27 % slow */
    uint32_t capture_delay_steps; /**< DUT-to-plant delay: H + a round trip, 17 by default */
    uint32_t pump_cmd_timeout_ms;
} OcL2LoopConfig;

/** @brief d936la6, the hoist pump, the reference controller and ECU, H = 16 plus a round trip. */
extern const OcL2LoopConfig OC_L2_LOOP_D936LA6;

typedef struct {
    uint64_t start;   /**< bridge ticks from step 0 */
    uint32_t width;   /**< bridge ticks */
    uint8_t channel;  /**< 0-5 */
    uint32_t due;     /**< the step it reaches the plant */
    float width_us_commanded; /**< what the ECU asked for, in its own microseconds */
} OcL2Capture;

/** @brief What the last counted injection was, for the trace, the oracle and the jitter budget. */
typedef struct {
    bool valid;
    double soi_err_deg;   /**< captured start - (its TDC - the ECU's start of injection) */
    double width_us;      /**< captured, in the bridge's time */
    double width_err_us;  /**< captured - commanded (in the ECU's microseconds) */
} OcL2Injection;

typedef struct {
    const OcL2LoopConfig *cfg;
    OcSignals sig;
    OtkPlayer player;

    OcL2Plant plant;
    HydraulicPumpState pump;
    McCtx mc;
    EcuWheel wheel;
    EcuCtx ecu;

    uint32_t t_ms;            /**< time at the start of the next step */
    double load_torque_nm;
    double pump_cmd_pct;
    bool pump_cmd_seen;
    uint32_t pump_cmd_ms;

    OtkCanQueue to_ecu;       /**< TSC1 from the controller, delivered at the ECU's next step */
    OtkCanQueue to_mc;        /**< EEC1 from the ECU and the operator's frames, delivered at the controller's next step */

    /* The edge model. */
    uint32_t offset32;        /**< the bridge's crank_offset */
    uint64_t next_tooth_u;    /**< unwrapped accumulator of the next crank edge */
    int64_t next_tooth_rev;
    uint32_t next_tooth_idx;
    uint64_t next_cam_u;
    uint32_t teeth_to_drop;
    double last_drop_signal;
    bool arm_pending;
    EcuArm arm;
    uint32_t teeth_seen;      /**< crank edges given to the ECU */

    OcL2Capture cap[OC_L2_CAPTURES];
    size_t n_cap;
    uint32_t captures_lost;   /**< the in-flight list was full (a broken run) */

    /* What the oracle and the trace see. */
    OcL2Injection last;
    bool first_injection_seen;
    double first_injection_rev;
    uint32_t first_injection_ms;
    uint32_t misfire_hist[1000]; /**< the plant's misfire count, a step each, for misfires_1s */
    uint32_t misfires_1s;
    double ecu_eec1_rpm;
    bool ecu_eec1_seen;

    /* For the jitter budget: every counted injection after the first second of FULL sync. */
    double *soi_err;          /**< caller's buffer, or NULL */
    size_t soi_cap;
    size_t soi_n;
    double width_err_max_us;
    double width_rel_err_max;
    double width_excess_max_us; /**< the largest |width error| - 0.3 % of the width (decision 14: within 1 us) */
    bool width_measured;
} OcL2Loop;

void oc_l2_loop_init(OcL2Loop *l, const OcL2LoopConfig *cfg, const OcScenario *scenario);

/** @brief Keeps every injection's start error in @p buf (up to @p cap) for the jitter budget. */
void oc_l2_loop_keep_errors(OcL2Loop *l, double *buf, size_t cap);

/** @brief One 1 ms step. Returns the frames the ECU sent this step (EEC1) in @p ecu_tx, at most @p cap. */
size_t oc_l2_loop_step(OcL2Loop *l, OtkCanFrame *ecu_tx, size_t cap);

/** @brief True once the scenario's `end` is reached. */
bool oc_l2_loop_done(const OcL2Loop *l);

/** @brief Simulated time, seconds. */
double oc_l2_loop_time_s(const OcL2Loop *l);

#endif /* OPEN_CRANK_L2_LOOP_H */
