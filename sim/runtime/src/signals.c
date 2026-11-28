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
    [OC_SIG_FAULT_ENGINE_DTC_RED] = "fault.engine_dtc_red",
    [OC_SIG_FAULT_ENGINE_DTC_AMBER] = "fault.engine_dtc_amber",
    [OC_SIG_TEST_DUT_HANG] = "test.dut_hang",
    [OC_SIG_FAULT_CRANK_TEETH_LOST] = "fault.crank_teeth_lost",
    [OC_SIG_LINK_MC] = OTK_LINK_SIGNAL_NAMES("mc"),
    [OC_SIG_LINK_ENGINE] = OTK_LINK_SIGNAL_NAMES("engine"),
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
    if (otk_link_signal(&s->link_mc, (OtkSignalId)OC_SIG_LINK_MC, (OtkSignalId)id, value) ||
        otk_link_signal(&s->link_engine, (OtkSignalId)OC_SIG_LINK_ENGINE, (OtkSignalId)id, value)) {
        return; /* fault.link.*: applied to the links by the bench each step */
    }
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
    case OC_SIG_FAULT_ENGINE_DTC_RED:
        s->fault_engine_dtc_red = on;
        break;
    case OC_SIG_FAULT_ENGINE_DTC_AMBER:
        s->fault_engine_dtc_amber = on;
        break;
    case OC_SIG_TEST_DUT_HANG:
        s->test_dut_hang = on;
        break;
    case OC_SIG_FAULT_CRANK_TEETH_LOST:
        s->fault_crank_teeth_lost = value;
        break;
    case OC_SIG_END:
    case OC_SIG_COUNT:
    default:
        break;
    }
}
