/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file trace.c
 * @brief CSV trace of a run. See trace.h.
 */

#include <opencrank/runtime/trace.h>

const char *oc_ecu_state_name(EngineEcuState s) {
    switch (s) {
    case ECU_STATE_OFF:
        return "OFF";
    case ECU_STATE_STOPPED:
        return "STOPPED";
    case ECU_STATE_CRANKING:
        return "CRANKING";
    case ECU_STATE_RUNNING:
        return "RUNNING";
    default:
        return "?";
    }
}

static const OtkTraceColumn COLUMNS[] = {
    { "t_s", OTK_COL_REAL, 3 },
    { "key_on", OTK_COL_INT, 0 },
    { "start", OTK_COL_INT, 0 },
    { "hoist_demand_pct", OTK_COL_REAL, 1 },
    { "hoist_load_bar", OTK_COL_REAL, 1 },
    { "rpm", OTK_COL_REAL, 2 },
    { "fuel_pct", OTK_COL_REAL, 2 },
    { "boost_frac", OTK_COL_REAL, 4 },
    { "starter", OTK_COL_INT, 0 },
    { "indicated_nm", OTK_COL_REAL, 1 },
    { "brake_nm", OTK_COL_REAL, 1 },
    { "load_nm", OTK_COL_REAL, 1 },
    { "ecu_state", OTK_COL_TEXT, 0 },
    { "ecu_speed_source", OTK_COL_TEXT, 0 },
    { "ecu_target_rpm", OTK_COL_REAL, 1 },
    { "ecu_tripped", OTK_COL_INT, 0 },
    { "controlling_sa", OTK_COL_UINT, 0 },
    { "pump_cmd_pct", OTK_COL_REAL, 1 },
    { "pump_disp_pct", OTK_COL_REAL, 1 },
    { "pump_bar", OTK_COL_REAL, 1 },
    { "coolant_c", OTK_COL_REAL, 2 },
    { "oil_kpa", OTK_COL_REAL, 1 },
    { "mc_state", OTK_COL_TEXT, 0 },
    { "mc_fault", OTK_COL_TEXT, 0 },
    { "mc_target_rpm", OTK_COL_REAL, 1 },
    { "mc_request_rpm", OTK_COL_REAL, 1 },
};

bool oc_trace_open(OtkTrace *t, FILE *fp) {
    return otk_trace_init(t, fp, COLUMNS, sizeof(COLUMNS) / sizeof(COLUMNS[0]));
}

void oc_trace_row(OtkTrace *t, const OcBench *b, const McCtx *mc) {
    const PlantState *p = &b->plant;
    const EngineEcu *e = &b->ecu;
    otk_trace_real(t, oc_bench_time_s(b));
    otk_trace_int(t, (int)b->sig.key_on);
    otk_trace_int(t, (int)b->sig.start);
    otk_trace_real(t, b->sig.hoist_demand_pct);
    otk_trace_real(t, b->sig.hoist_load_bar);
    otk_trace_real(t, p->rpm);
    otk_trace_real(t, p->fuel_pct);
    otk_trace_real(t, p->boost_frac);
    otk_trace_int(t, (int)e->starter_engaged);
    otk_trace_real(t, p->indicated_torque_nm);
    otk_trace_real(t, p->brake_torque_nm);
    otk_trace_real(t, b->load_torque_nm);
    otk_trace_text(t, oc_ecu_state_name(e->state));
    otk_trace_text(t, (e->source == ECU_SPEED_SOURCE_REQUEST) ? "REQUEST" : "LOW_IDLE");
    otk_trace_real(t, e->target_rpm);
    otk_trace_int(t, (int)e->overspeed_tripped);
    otk_trace_uint(t, e->controlling_address);
    otk_trace_real(t, b->pump_cmd_applied_pct);
    otk_trace_real(t, 100.0 * b->pump.displacement_frac);
    otk_trace_real(t, b->pump.pressure_bar);
    otk_trace_real(t, p->coolant_temp_c);
    otk_trace_real(t, p->oil_pressure_kpa);
    if (mc != NULL) {
        otk_trace_text(t, mc_state_name(mc->state));
        otk_trace_text(t, mc_fault_name(mc->fault));
        otk_trace_real(t, (double)mc->target_rpm);
        otk_trace_real(t, (double)mc->requested_rpm);
    } else {
        otk_trace_empty(t);
        otk_trace_empty(t);
        otk_trace_empty(t);
        otk_trace_empty(t);
    }
    (void)otk_trace_end_row(t);
}
