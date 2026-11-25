/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939.c
 * @brief SAE J1939 codec for TSC1, EEC1, ET1 and EFL/P1. See j1939.h.
 */

#include <opencrank/j1939.h>

#include "j1939_scaling.h"

#define PDU2_MIN_PF 240U

static uint8_t pf_of(uint32_t pgn) {
    return (uint8_t)((pgn >> 8) & 0xFFU);
}

uint32_t j1939_make_id(uint8_t priority, uint32_t pgn, uint8_t dest, uint8_t src) {
    uint32_t id = (((uint32_t)priority & 0x07U) << 26);
    if (pf_of(pgn) < PDU2_MIN_PF) {
        id |= ((pgn & 0x3FF00U) << 8) | ((uint32_t)dest << 8);
    } else {
        id |= (pgn & 0x3FFFFU) << 8;
    }
    return id | (uint32_t)src;
}

uint32_t j1939_id_pgn(uint32_t id) {
    uint32_t pgn = (id >> 8) & 0x3FFFFU;
    if (pf_of(pgn) < PDU2_MIN_PF) {
        pgn &= 0x3FF00U;
    }
    return pgn;
}

uint8_t j1939_id_source(uint32_t id) {
    return (uint8_t)(id & 0xFFU);
}

uint8_t j1939_id_dest(uint32_t id) {
    const uint32_t pgn = (id >> 8) & 0x3FFFFU;
    return (pf_of(pgn) < PDU2_MIN_PF) ? (uint8_t)((id >> 8) & 0xFFU) : (uint8_t)J1939_ADDR_GLOBAL;
}

uint8_t j1939_id_priority(uint32_t id) {
    return (uint8_t)((id >> 26) & 0x07U);
}

/* ---------------------------------------------------------------- TSC1 */

uint8_t j1939_tsc1_checksum(const J1939Frame *f, uint8_t counter) {
    uint32_t sum = (uint32_t)counter & 0x0FU;
    for (uint32_t i = 0U; i < 7U; i++) {
        sum += (uint32_t)f->data[i];
    }
    sum += f->id & 0xFFU;
    sum += (f->id >> 8U) & 0xFFU;
    sum += (f->id >> 16U) & 0xFFU;
    sum += (f->id >> 24U) & 0xFFU;
    return (uint8_t)(((sum >> 4U) + sum) & 0x0FU);
}

void j1939_encode_tsc1(const J1939Tsc1 *m, uint8_t src, uint8_t dest, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_CONTROL, J1939_PGN_TSC1, dest, src));
    f->data[0] = (uint8_t)(((uint8_t)m->override_mode & 0x03U) | (uint8_t)((m->speed_conditions & 0x03U) << 2) |
                           (uint8_t)((m->mode_priority & 0x03U) << 4) | 0xC0U);
    if (m->requested_speed_valid) {
        j1939_put_u16(&f->data[1], j1939_to_u16(m->requested_speed_rpm, 0.125f, 0.0f));
    }
    if (m->requested_torque_valid) {
        f->data[3] = j1939_to_u8(m->requested_torque_pct, 1.0f, -125.0f);
    }
    const uint8_t counter = (uint8_t)(m->message_counter & 0x0FU);
    f->data[7] = (uint8_t)((uint8_t)(j1939_tsc1_checksum(f, counter) << 4U) | counter);
}

bool j1939_decode_tsc1(const J1939Frame *f, J1939Tsc1 *m) {
    if (!j1939_frame_is(f, J1939_PGN_TSC1)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    switch (f->data[0] & 0x03U) {
        case (uint8_t)J1939_OVERRIDE_DISABLED:
            m->override_mode = J1939_OVERRIDE_DISABLED;
            break;
        case (uint8_t)J1939_OVERRIDE_SPEED:
            m->override_mode = J1939_OVERRIDE_SPEED;
            break;
        case (uint8_t)J1939_OVERRIDE_TORQUE:
            m->override_mode = J1939_OVERRIDE_TORQUE;
            break;
        case (uint8_t)J1939_OVERRIDE_SPEED_TORQUE_LIMIT:
            m->override_mode = J1939_OVERRIDE_SPEED_TORQUE_LIMIT;
            break;
        default:
            m->override_mode = J1939_OVERRIDE_DISABLED;
            break;
    }
    m->speed_conditions = (uint8_t)((f->data[0] >> 2) & 0x03U);
    m->mode_priority = (uint8_t)((f->data[0] >> 4) & 0x03U);
    m->requested_speed_valid = j1939_from_u16(j1939_get_u16(&f->data[1]), 0.125f, 0.0f, &m->requested_speed_rpm);
    m->requested_torque_valid = j1939_from_u8(f->data[3], 1.0f, -125.0f, &m->requested_torque_pct);
    m->message_counter = (uint8_t)(f->data[7] & 0x0FU);
    m->checksum_valid = (uint8_t)((f->data[7] >> 4U) & 0x0FU) == j1939_tsc1_checksum(f, m->message_counter);
    return true;
}

/* ---------------------------------------------------------------- EEC1 */

void j1939_encode_eec1(const J1939Eec1 *m, uint8_t src, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_CONTROL, J1939_PGN_EEC1, J1939_ADDR_GLOBAL, src));
    f->data[0] = (uint8_t)((m->torque_mode & 0x0FU) | 0xF0U);
    if (m->driver_demand_torque_valid) {
        f->data[1] = j1939_to_u8(m->driver_demand_torque_pct, 1.0f, -125.0f);
    }
    if (m->actual_torque_valid) {
        f->data[2] = j1939_to_u8(m->actual_torque_pct, 1.0f, -125.0f);
    }
    if (m->engine_speed_valid) {
        j1939_put_u16(&f->data[3], j1939_to_u16(m->engine_speed_rpm, 0.125f, 0.0f));
    }
    f->data[5] = m->controlling_address;
}

bool j1939_decode_eec1(const J1939Frame *f, J1939Eec1 *m) {
    if (!j1939_frame_is(f, J1939_PGN_EEC1)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    m->torque_mode = (uint8_t)(f->data[0] & 0x0FU);
    m->driver_demand_torque_valid = j1939_from_u8(f->data[1], 1.0f, -125.0f, &m->driver_demand_torque_pct);
    m->actual_torque_valid = j1939_from_u8(f->data[2], 1.0f, -125.0f, &m->actual_torque_pct);
    m->engine_speed_valid = j1939_from_u16(j1939_get_u16(&f->data[3]), 0.125f, 0.0f, &m->engine_speed_rpm);
    m->controlling_address = f->data[5];
    return true;
}

/* ---------------------------------------------------------------- ET1 */

void j1939_encode_et1(const J1939Et1 *m, uint8_t src, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_ET1, J1939_ADDR_GLOBAL, src));
    if (m->coolant_temp_valid) {
        f->data[0] = j1939_to_u8(m->coolant_temp_c, 1.0f, -40.0f);
    }
}

bool j1939_decode_et1(const J1939Frame *f, J1939Et1 *m) {
    if (!j1939_frame_is(f, J1939_PGN_ET1)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    m->coolant_temp_valid = j1939_from_u8(f->data[0], 1.0f, -40.0f, &m->coolant_temp_c);
    return true;
}

/* ---------------------------------------------------------------- EFL/P1 */

void j1939_encode_eflp1(const J1939Eflp1 *m, uint8_t src, J1939Frame *f) {
    j1939_frame_init(f, j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_EFLP1, J1939_ADDR_GLOBAL, src));
    if (m->oil_pressure_valid) {
        f->data[3] = j1939_to_u8(m->oil_pressure_kpa, 4.0f, 0.0f);
    }
}

bool j1939_decode_eflp1(const J1939Frame *f, J1939Eflp1 *m) {
    if (!j1939_frame_is(f, J1939_PGN_EFLP1)) {
        return false; /* DEV-003: PGN guard before the output is touched */
    }
    m->oil_pressure_valid = j1939_from_u8(f->data[3], 4.0f, 0.0f, &m->oil_pressure_kpa);
    return true;
}
