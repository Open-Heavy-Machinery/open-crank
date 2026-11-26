/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file accept.c
 * @brief open-crank's observables for the acceptance oracle. See accept.h.
 */

#include <opencrank/accept.h>

#include <opencrank/can_otk.h>
#include <opencrank/j1939.h>
#include <opencrank/j1939_diag.h>
#include <opencrank/oc_msgs.h>

const OtkOracleObservable OC_ACCEPT_OBSERVABLES[OC_ACCEPT_OBSERVABLE_COUNT] = {
    [OC_ACCEPT_TSC1_SPEED_CONTROL] = { "tsc1_speed_control", OC_ACCEPT_STALE_MS * OTK_NS_PER_MS, { false, 0.0 } },
    [OC_ACCEPT_TSC1_SPEED_RPM] = { "tsc1_speed_rpm", OC_ACCEPT_STALE_MS * OTK_NS_PER_MS, { true, 0.0 } },
    [OC_ACCEPT_PUMP_CMD_PCT] = { "pump_cmd_pct", OC_ACCEPT_STALE_MS * OTK_NS_PER_MS, { false, 0.0 } },
    [OC_ACCEPT_TSC1_OVERRIDE] = { "tsc1_override", OC_ACCEPT_STALE_MS * OTK_NS_PER_MS, { false, 0.0 } },
    [OC_ACCEPT_TSC1_INTACT] = { "tsc1_intact", OC_ACCEPT_STALE_MS * OTK_NS_PER_MS, { false, 1.0 } },
    [OC_ACCEPT_DM1_RED_STOP] = { "dm1_red_stop", OC_ACCEPT_DM1_STALE_MS * OTK_NS_PER_MS, { false, 0.0 } },
    [OC_ACCEPT_DM1_SPN] = { "dm1_spn", OC_ACCEPT_DM1_STALE_MS * OTK_NS_PER_MS, { true, 0.0 } },
};

void oc_accept_init(OcAccept *a, uint8_t dut, OtkOracleTable *table) {
    a->dut = dut;
    a->tsc1_seen = false;
    a->tsc1_counter = 0U;
    table->observables = OC_ACCEPT_OBSERVABLES;
    table->count = OC_ACCEPT_OBSERVABLE_COUNT;
    table->decode = oc_accept_decode;
    table->ctx = a;
}

size_t oc_accept_decode(void *ctx, const OtkCanFrame *f, OtkOracleUpdate *out, size_t cap) {
    OcAccept *a = (OcAccept *)ctx;
    J1939Frame jf;
    if (!oc_frame_from_otk(f, &jf) || j1939_id_source(jf.id) != a->dut) {
        return 0U;
    }

    J1939Tsc1 tsc1;
    OcPumpCmd pump;
    if (j1939_decode_tsc1(&jf, &tsc1)) {
        if (j1939_id_dest(jf.id) != J1939_ADDR_ENGINE_1 || cap < 4U) {
            return 0U;
        }
        /* SPN 4206/4207: intact if the checksum is right and the counter moved by exactly one. */
        const bool next = !a->tsc1_seen || tsc1.message_counter == (uint8_t)((a->tsc1_counter + 1U) & 0x0FU);
        a->tsc1_seen = true;
        a->tsc1_counter = tsc1.message_counter;
        out[3] = (OtkOracleUpdate){ OC_ACCEPT_TSC1_INTACT, { false, (tsc1.checksum_valid && next) ? 1.0 : 0.0 } };
        const bool speed = tsc1.override_mode == J1939_OVERRIDE_SPEED && tsc1.requested_speed_valid;
        out[0] = (OtkOracleUpdate){ OC_ACCEPT_TSC1_SPEED_CONTROL, { false, speed ? 1.0 : 0.0 } };
        out[1] = (OtkOracleUpdate){ OC_ACCEPT_TSC1_SPEED_RPM,
                                   { !speed, speed ? (double)tsc1.requested_speed_rpm : 0.0 } };
        const bool overriding = tsc1.override_mode != J1939_OVERRIDE_DISABLED;
        out[2] = (OtkOracleUpdate){ OC_ACCEPT_TSC1_OVERRIDE, { false, overriding ? 1.0 : 0.0 } };
        return 4U;
    }
    if (oc_decode_pump_cmd(&jf, &pump)) {
        if (cap < 1U) {
            return 0U;
        }
        out[0] = (OtkOracleUpdate){ OC_ACCEPT_PUMP_CMD_PCT,
                                   { false, pump.displacement_valid ? (double)pump.displacement_pct : 0.0 } };
        return 1U;
    }
    J1939Dm1 dm1;
    if (jf.dlc == 8U && j1939_id_pgn(jf.id) == J1939_PGN_DM1 && j1939_decode_dm1(jf.data, 8U, &dm1)) {
        if (cap < 2U) {
            return 0U;
        }
        out[0] = (OtkOracleUpdate){ OC_ACCEPT_DM1_RED_STOP, { false, dm1.red_stop == J1939_LAMP_ON ? 1.0 : 0.0 } };
        out[1] = (OtkOracleUpdate){ OC_ACCEPT_DM1_SPN, { dm1.n_dtc == 0U, dm1.n_dtc == 0U ? 0.0 : (double)dm1.dtc[0].spn } };
        return 2U;
    }
    return 0U;
}
