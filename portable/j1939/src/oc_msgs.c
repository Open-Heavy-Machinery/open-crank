/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oc_msgs.c
 * @brief open-crank bench messages (not SAE J1939). See oc_msgs.h.
 */

#include <opencrank/oc_msgs.h>

#include "j1939_scaling.h"

void oc_encode_operator(const OcOperator *m, uint8_t src, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_INFO, OC_PGN_OPERATOR, J1939_ADDR_GLOBAL, src));
    if (m->target_speed_valid) {
        j1939_put_u16(&f->data[0], j1939_to_u16(m->target_speed_rpm, 0.125f, 0.0f));
    }
    if (m->hoist_demand_valid) {
        f->data[2] = j1939_to_u8(m->hoist_demand_pct, 0.4f, 0.0f);
    }
    f->data[3] = (uint8_t)(0xFCU | (m->fault_reset_pressed ? 0x01U : 0x00U));
}

bool oc_decode_operator(const J1939Frame *f, OcOperator *m) {
    if (!j1939_frame_is(f, OC_PGN_OPERATOR)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    m->target_speed_valid = j1939_from_u16(j1939_get_u16(&f->data[0]), 0.125f, 0.0f, &m->target_speed_rpm);
    m->hoist_demand_valid = j1939_from_u8(f->data[2], 0.4f, 0.0f, &m->hoist_demand_pct);
    m->fault_reset_pressed = (f->data[3] & 0x03U) == 0x01U;
    return true;
}

void oc_encode_pump_cmd(const OcPumpCmd *m, uint8_t src, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_CONTROL, OC_PGN_PUMP_CMD, J1939_ADDR_GLOBAL, src));
    if (m->displacement_valid) {
        f->data[0] = j1939_to_u8(m->displacement_pct, 0.4f, 0.0f);
    }
}

bool oc_decode_pump_cmd(const J1939Frame *f, OcPumpCmd *m) {
    if (!j1939_frame_is(f, OC_PGN_PUMP_CMD)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    m->displacement_valid = j1939_from_u8(f->data[0], 0.4f, 0.0f, &m->displacement_pct);
    return true;
}
