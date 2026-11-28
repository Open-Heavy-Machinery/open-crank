/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file signals.h
 * @brief Named inputs of the simulated machine: operator controls, the
 *        environment, and fault injection. Scenarios set them by name.
 */

#ifndef OPEN_CRANK_RUNTIME_SIGNALS_H
#define OPEN_CRANK_RUNTIME_SIGNALS_H

#include <stdbool.h>

#include <opentick/link.h>
#include <opentick/signals.h>

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
    OC_SIG_FAULT_ENGINE_DTC_RED,   /**< fault.engine_dtc_red: the engine reports low oil pressure, red stop lamp, 0/1 */
    OC_SIG_FAULT_ENGINE_DTC_AMBER, /**< fault.engine_dtc_amber: the engine reports high coolant temperature, amber lamp, 0/1 */
    OC_SIG_TEST_DUT_HANG,          /**< test.dut_hang: the bench sends the test frame that hangs a hang-test build, 0/1 */
    OC_SIG_FAULT_CRANK_TEETH_LOST, /**< fault.crank_teeth_lost: Level 2 only: the next N crank edges never reach the ECU */
    OC_SIG_LINK_MC,                /**< fault.link.mc.*: the six faults of the link controller -> machine */
    OC_SIG_LINK_ENGINE = OC_SIG_LINK_MC + (int)OTK_LINK_SIGNAL_COUNT, /**< fault.link.engine.*: engine -> controller */
    OC_SIG_END = OC_SIG_LINK_ENGINE + (int)OTK_LINK_SIGNAL_COUNT,     /**< end: marks the end of the scenario (value ignored) */
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
    bool fault_engine_dtc_red;
    bool fault_engine_dtc_amber;
    bool test_dut_hang;
    double fault_crank_teeth_lost; /**< Level 2 only; the Level 1 bench ignores it */

    OtkLinkFaults link_mc;       /**< fault.link.mc.*: what the controller sends to the machine */
    OtkLinkFaults link_engine;   /**< fault.link.engine.*: what the engine sends to the controller */
} OcSignals;

/** @brief The signals above, registered with open-tick (`end` excluded: open-tick reserves it). */
extern const OtkSignalTable OC_SIGNAL_TABLE;

/** @brief Looks a signal up by its scenario name. */
bool oc_signal_from_name(const char *name, OcSignalId *id);

/** @brief Scenario name of a signal. */
const char *oc_signal_name(OcSignalId id);

/** @brief Sets one signal. Booleans are true for any non-zero value. */
void oc_signals_set(OcSignals *s, OcSignalId id, double value);

#endif /* OPEN_CRANK_RUNTIME_SIGNALS_H */
