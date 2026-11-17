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

void oc_trace_header(FILE *fp) {
    fputs("t_s,key_on,start,hoist_demand_pct,hoist_load_bar,"
          "rpm,fuel_pct,boost_frac,starter,indicated_nm,brake_nm,load_nm,"
          "ecu_state,ecu_speed_source,ecu_target_rpm,ecu_tripped,controlling_sa,"
          "pump_cmd_pct,pump_disp_pct,pump_bar,coolant_c,oil_kpa,"
          "mc_state,mc_fault,mc_target_rpm,mc_request_rpm\n",
          fp);
}

void oc_trace_row(FILE *fp, const OcBench *b, const McCtx *mc) {
    const PlantState *p = &b->plant;
    const EngineEcu *e = &b->ecu;
    fprintf(fp, "%.3f,%d,%d,%.1f,%.1f,%.2f,%.2f,%.4f,%d,%.1f,%.1f,%.1f,%s,%s,%.1f,%d,%u,%.1f,%.1f,%.1f,%.2f,%.1f,",
            oc_bench_time_s(b), (int)b->sig.key_on, (int)b->sig.start, b->sig.hoist_demand_pct,
            b->sig.hoist_load_bar, p->rpm, p->fuel_pct, p->boost_frac, (int)e->starter_engaged,
            p->indicated_torque_nm, p->brake_torque_nm, b->load_torque_nm, oc_ecu_state_name(e->state),
            (e->source == ECU_SPEED_SOURCE_REQUEST) ? "REQUEST" : "LOW_IDLE", e->target_rpm,
            (int)e->overspeed_tripped, (unsigned)e->controlling_address, b->pump_cmd_applied_pct,
            100.0 * b->pump.displacement_frac, b->pump.pressure_bar, p->coolant_temp_c, p->oil_pressure_kpa);
    if (mc != NULL) {
        fprintf(fp, "%s,%s,%.1f,%.1f\n", mc_state_name(mc->state), mc_fault_name(mc->fault), (double)mc->target_rpm,
                (double)mc->requested_rpm);
    } else {
        fputs(",,,\n", fp);
    }
}
