/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file signals.c
 * @brief Named inputs of the simulated machine. See signals.h.
 */

#include <opencrank/runtime/signals.h>


static const char *const NAMES[OC_SIG_END] = {
    [OC_SIG_KEY_ON] = "key_on",
    [OC_SIG_START] = "start",
    [OC_SIG_TARGET_RPM] = "target_rpm",
    [OC_SIG_HOIST_DEMAND_PCT] = "hoist_demand_pct",
    [OC_SIG_HOIST_LOAD_BAR] = "hoist_load_bar",
    [OC_SIG_FAULT_RESET] = "fault_reset",
    [OC_SIG_FAULT_MC_TX_LOST] = "fault.mc_tx_lost",
    [OC_SIG_FAULT_ENGINE_TX_LOST] = "fault.engine_tx_lost",
    [OC_SIG_FAULT_ECU_IGNORES_TSC1] = "fault.ecu_ignores_tsc1",
    [OC_SIG_FAULT_OVERRUN_TORQUE_NM] = "fault.overrun_torque_nm",
};

const OtkSignalTable OC_SIGNAL_TABLE = { NAMES, OC_SIG_END };

bool oc_signal_from_name(const char *name, OcSignalId *id) {
    OtkSignalId otk;
    if (!otk_signal_find(&OC_SIGNAL_TABLE, name, &otk)) {
        return false;
    }
    *id = (otk == OTK_SIGNAL_END) ? OC_SIG_END : (OcSignalId)otk;
    return true;
}

const char *oc_signal_name(OcSignalId id) {
    if (id == OC_SIG_END) {
        return OTK_SIGNAL_END_NAME;
    }
    return ((int)id >= 0 && id < OC_SIG_END) ? NAMES[id] : "?";
}

void oc_signals_set(OcSignals *s, OcSignalId id, double value) {
    const bool on = value != 0.0;
    switch (id) {
    case OC_SIG_KEY_ON:
        s->key_on = on;
        break;
    case OC_SIG_START:
        s->start = on;
        break;
    case OC_SIG_TARGET_RPM:
        s->target_rpm = value;
        break;
    case OC_SIG_HOIST_DEMAND_PCT:
        s->hoist_demand_pct = value;
        break;
    case OC_SIG_HOIST_LOAD_BAR:
        s->hoist_load_bar = value;
        break;
    case OC_SIG_FAULT_RESET:
        s->fault_reset = on;
        break;
    case OC_SIG_FAULT_MC_TX_LOST:
        s->fault_mc_tx_lost = on;
        break;
    case OC_SIG_FAULT_ENGINE_TX_LOST:
        s->fault_engine_tx_lost = on;
        break;
    case OC_SIG_FAULT_ECU_IGNORES_TSC1:
        s->fault_ecu_ignores_tsc1 = on;
        break;
    case OC_SIG_FAULT_OVERRUN_TORQUE_NM:
        s->fault_overrun_torque_nm = value;
        break;
    case OC_SIG_END:
    case OC_SIG_COUNT:
    default:
        break;
    }
}
