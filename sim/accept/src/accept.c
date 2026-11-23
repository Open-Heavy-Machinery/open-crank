/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file accept.c
 * @brief open-crank's observables for the acceptance oracle. See accept.h.
 */

#include <opencrank/accept.h>

#include <opencrank/can_otk.h>
#include <opencrank/j1939.h>
#include <opencrank/oc_msgs.h>

const OcOracleObservable OC_ACCEPT_OBSERVABLES[OC_ACCEPT_OBSERVABLE_COUNT] = {
    [OC_ACCEPT_TSC1_SPEED_CONTROL] = { "tsc1_speed_control", OC_ACCEPT_STALE_MS, { false, 0.0 } },
    [OC_ACCEPT_TSC1_SPEED_RPM] = { "tsc1_speed_rpm", OC_ACCEPT_STALE_MS, { true, 0.0 } },
    [OC_ACCEPT_PUMP_CMD_PCT] = { "pump_cmd_pct", OC_ACCEPT_STALE_MS, { false, 0.0 } },
};

void oc_accept_init(OcAccept *a, uint8_t dut, OcOracleTable *table) {
    a->dut = dut;
    table->observables = OC_ACCEPT_OBSERVABLES;
    table->count = OC_ACCEPT_OBSERVABLE_COUNT;
    table->decode = oc_accept_decode;
    table->ctx = a;
}

size_t oc_accept_decode(void *ctx, const OtkCanFrame *f, OcOracleUpdate *out, size_t cap) {
    const OcAccept *a = (const OcAccept *)ctx;
    J1939Frame jf;
    if (!oc_frame_from_otk(f, &jf) || j1939_id_source(jf.id) != a->dut) {
        return 0U;
    }

    J1939Tsc1 tsc1;
    OcPumpCmd pump;
    if (j1939_decode_tsc1(&jf, &tsc1)) {
        if (j1939_id_dest(jf.id) != J1939_ADDR_ENGINE_1 || cap < 2U) {
            return 0U;
        }
        const bool speed = tsc1.override_mode == J1939_OVERRIDE_SPEED && tsc1.requested_speed_valid;
        out[0] = (OcOracleUpdate){ OC_ACCEPT_TSC1_SPEED_CONTROL, { false, speed ? 1.0 : 0.0 } };
        out[1] = (OcOracleUpdate){ OC_ACCEPT_TSC1_SPEED_RPM,
                                   { !speed, speed ? (double)tsc1.requested_speed_rpm : 0.0 } };
        return 2U;
    }
    if (oc_decode_pump_cmd(&jf, &pump)) {
        if (cap < 1U) {
            return 0U;
        }
        out[0] = (OcOracleUpdate){ OC_ACCEPT_PUMP_CMD_PCT,
                                   { false, pump.displacement_valid ? (double)pump.displacement_pct : 0.0 } };
        return 1U;
    }
    return 0U;
}
