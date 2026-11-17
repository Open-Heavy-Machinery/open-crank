/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file signals.h
 * @brief Named inputs of the simulated machine: operator controls, the
 *        environment, and fault injection. Scenarios set them by name.
 */

#ifndef OPEN_CRANK_RUNTIME_SIGNALS_H
#define OPEN_CRANK_RUNTIME_SIGNALS_H

#include <stdbool.h>

/** @brief Every signal a scenario can set. Names are listed in signals.c. */
typedef enum {
    OC_SIG_KEY_ON = 0,             /**< key_on: key switch, 0/1 */
    OC_SIG_START,                  /**< start: key held in START, 0/1 */
    OC_SIG_TARGET_RPM,             /**< target_rpm: operator hand throttle [rpm], 0 = not set */
    OC_SIG_HOIST_DEMAND_PCT,       /**< hoist_demand_pct: operator hoist lever [%] */
    OC_SIG_HOIST_LOAD_BAR,         /**< hoist_load_bar: pressure the load on the hook requires [bar] */
    OC_SIG_FAULT_RESET,            /**< fault_reset: operator fault-reset button, 0/1 */
    OC_SIG_FAULT_MC_TX_LOST,       /**< fault.mc_tx_lost: frames from the device under test are lost, 0/1 */
    OC_SIG_FAULT_ENGINE_TX_LOST,   /**< fault.engine_tx_lost: the engine stops transmitting, 0/1 */
    OC_SIG_FAULT_ECU_IGNORES_TSC1, /**< fault.ecu_ignores_tsc1: engine ECU ignores speed requests, 0/1 */
    OC_SIG_FAULT_OVERRUN_TORQUE_NM,/**< fault.overrun_torque_nm: external torque driving the crankshaft [Nm] */
    OC_SIG_END,                    /**< end: marks the end of the scenario (value ignored) */
    OC_SIG_COUNT
} OcSignalId;

/** @brief Current values of the signals. */
typedef struct {
    bool key_on;
    bool start;
    double target_rpm;
    double hoist_demand_pct;
    double hoist_load_bar;
    bool fault_reset;

    bool fault_mc_tx_lost;
    bool fault_engine_tx_lost;
    bool fault_ecu_ignores_tsc1;
    double fault_overrun_torque_nm;
} OcSignals;

/** @brief Looks a signal up by its scenario name. */
bool oc_signal_from_name(const char *name, OcSignalId *id);

/** @brief Scenario name of a signal. */
const char *oc_signal_name(OcSignalId id);

/** @brief Sets one signal. Booleans are true for any non-zero value. */
void oc_signals_set(OcSignals *s, OcSignalId id, double value);

#endif /* OPEN_CRANK_RUNTIME_SIGNALS_H */
