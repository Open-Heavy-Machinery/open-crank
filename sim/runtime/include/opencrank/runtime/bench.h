/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bench.h
 * @brief The simulated machine as seen from the CAN bus.
 *
 * The bench owns the plant, the simulated engine ECU and the hoist pump. It
 * decodes the frames a Level 1 device under test sends (TSC1, OC_PUMP_CMD),
 * advances the physics by one fixed step, and returns the frames the
 * simulated machine transmits (EEC1, ET1, EFL/P1, OC_OPERATOR).
 *
 * Time: the bench owns simulated time as an integer millisecond count and
 * never reads a clock. Whoever calls oc_bench_step() decides how fast time
 * passes: a free-running loop (tests, CI), a wall-clock-paced loop (SIL demo)
 * or, later, an emulator (virtual time).
 */

#ifndef OPEN_CRANK_RUNTIME_BENCH_H
#define OPEN_CRANK_RUNTIME_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/engine_ecu.h>
#include <opencrank/j1939.h>
#include <opencrank/loads/hydraulic_pump.h>
#include <opencrank/plant.h>
#include <opencrank/runtime/scenario.h>
#include <opencrank/runtime/signals.h>

/** @brief What the bench simulates, and how. */
typedef struct {
    const PlantParams *plant;
    const EngineEcuParams *ecu;
    const HydraulicPumpParams *pump;
    uint32_t step_ms;               /**< physics step */
    uint32_t pump_cmd_timeout_ms;   /**< a stale pump command counts as zero (solenoid de-energised) */
    uint32_t eec1_period_ms;
    uint32_t eflp1_period_ms;
    uint32_t et1_period_ms;
    uint32_t operator_period_ms;
} OcBenchConfig;

/** @brief The reference configuration: d936la6 plant and ECU, hoist pump. */
extern const OcBenchConfig OC_BENCH_D936LA6;

typedef struct {
    const OcBenchConfig *cfg;
    OcSignals sig;               /**< operator, environment and fault-injection inputs */
    const OcScenario *scenario;  /**< optional: drives sig over time */
    size_t scenario_next;

    PlantState plant;
    EngineEcu ecu;
    HydraulicPumpState pump;

    uint32_t t_ms;               /**< simulated time at the start of the next step */
    double pump_cmd_pct;         /**< last received displacement command */
    bool pump_cmd_seen;
    uint32_t pump_cmd_ms;
    double pump_cmd_applied_pct; /**< command actually applied (after timeout) */
    double load_torque_nm;       /**< total load torque applied in the last step */

    uint32_t rx_frames;          /**< frames accepted from the device under test */
    uint32_t rx_dropped;         /**< frames dropped by fault injection */
} OcBench;

void oc_bench_init(OcBench *b, const OcBenchConfig *cfg);

/** @brief Lets a scenario drive the signals. Events apply at the start of the step they fall in. */
void oc_bench_set_scenario(OcBench *b, const OcScenario *s);

/** @brief True once the scenario's `end` time is reached (never true without a scenario or `end`). */
bool oc_bench_scenario_done(const OcBench *b);

/** @brief Hands the bench one frame received from the device under test. */
void oc_bench_rx(OcBench *b, const J1939Frame *f);

/**
 * @brief Advances the simulated machine by one step.
 * @return number of frames written to @p tx (at most @p cap) to put on the bus.
 */
size_t oc_bench_step(OcBench *b, J1939Frame *tx, size_t cap);

/** @brief Simulated time in seconds. */
double oc_bench_time_s(const OcBench *b);

#endif /* OPEN_CRANK_RUNTIME_BENCH_H */
