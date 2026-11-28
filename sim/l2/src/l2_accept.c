/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file l2_accept.c
 * @brief Level 2 observables for the acceptance oracle. See l2_accept.h.
 */

#include <opencrank/l2/l2_accept.h>

#include <string.h>

#include <opencrank/can_otk.h>
#include <opencrank/j1939.h>

#define PLANT_STALE_NS (50U * OTK_NS_PER_MS) /* sent every step: stale only if the runner stops sending */
#define EEC1_STALE_NS (200U * OTK_NS_PER_MS) /* the controller's own EEC1 timeout */

const OtkOracleObservable OC_L2_OBSERVABLES[OC_L2_OBS_COUNT] = {
    [OC_L2_OBS_PLANT_RPM] = { "plant_rpm", PLANT_STALE_NS, { false, 0.0 } },
    [OC_L2_OBS_ECU_RPM] = { "ecu_rpm", EEC1_STALE_NS, { true, 0.0 } },
    [OC_L2_OBS_MISPLACED] = { "misplaced", PLANT_STALE_NS, { false, 0.0 } },
    [OC_L2_OBS_MISFIRES_1S] = { "misfires_1s", PLANT_STALE_NS, { false, 0.0 } },
    [OC_L2_OBS_FIRST_INJECTION_REV] = { "first_injection_rev", PLANT_STALE_NS, { true, 0.0 } },
    [OC_L2_OBS_SOI_ERR_DEG] = { "soi_err_deg", PLANT_STALE_NS, { true, 0.0 } },
    [OC_L2_OBS_FUEL_PCT] = { "fuel_pct", PLANT_STALE_NS, { false, 0.0 } },
};

static size_t decode(void *ctx, const OtkCanFrame *f, OtkOracleUpdate *out, size_t cap) {
    const uint8_t ecu = *(const uint8_t *)ctx;
    if (cap < 1U) {
        return 0U;
    }
    if ((f->flags & OTK_CAN_EXTENDED) == 0U) {
        const uint32_t i = f->id - OC_L2_OBS_ID;
        if (f->id < OC_L2_OBS_ID || i >= (uint32_t)OC_L2_OBS_COUNT || i == (uint32_t)OC_L2_OBS_ECU_RPM) {
            return 0U;
        }
        double v = 0.0;
        if (f->len == 8U) {
            memcpy(&v, f->data, sizeof(v));
        }
        out[0] = (OtkOracleUpdate){ i, { f->len != 8U, v } };
        return 1U;
    }
    J1939Frame jf;
    J1939Eec1 m;
    if (oc_frame_from_otk(f, &jf) && j1939_id_source(jf.id) == ecu && j1939_decode_eec1(&jf, &m)) {
        out[0] = (OtkOracleUpdate){ OC_L2_OBS_ECU_RPM, { !m.engine_speed_valid, (double)m.engine_speed_rpm } };
        return 1U;
    }
    return 0U;
}

void oc_l2_accept_init(OtkOracleTable *table, const uint8_t *ecu_address) {
    table->observables = OC_L2_OBSERVABLES;
    table->count = OC_L2_OBS_COUNT;
    table->decode = decode;
    table->ctx = (void *)(uintptr_t)ecu_address;
}

static void obs(OtkCanFrame *f, OcL2Observable i, bool gap, double v) {
    memset(f, 0, sizeof(*f));
    f->id = OC_L2_OBS_ID + (uint32_t)i;
    if (!gap) {
        f->len = 8U;
        memcpy(f->data, &v, sizeof(v));
    }
}

size_t oc_l2_observation_frames(const OcL2Loop *l, OtkCanFrame *out, size_t cap) {
    if (cap < 6U) {
        return 0U;
    }
    obs(&out[0], OC_L2_OBS_PLANT_RPM, false, l->plant.plant.rpm);
    obs(&out[1], OC_L2_OBS_MISPLACED, false, (double)l->plant.misplaced);
    obs(&out[2], OC_L2_OBS_MISFIRES_1S, false, (double)l->misfires_1s);
    obs(&out[3], OC_L2_OBS_FIRST_INJECTION_REV, !l->first_injection_seen, l->first_injection_rev);
    obs(&out[4], OC_L2_OBS_SOI_ERR_DEG, !l->last.valid, l->last.soi_err_deg);
    obs(&out[5], OC_L2_OBS_FUEL_PCT, false, oc_l2_fuel_mean(&l->plant));
    return 6U;
}
