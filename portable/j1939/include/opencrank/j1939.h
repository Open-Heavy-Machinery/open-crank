/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939.h
 * @brief SAE J1939 codec for the v0.1 message set: TSC1, EEC1, ET1, EFL/P1.
 *
 * Portable zone: depends only on <stdint.h>, <stdbool.h>, <stddef.h>. Physical
 * values are float. Encoders saturate (out-of-range values clamp, never wrap)
 * and send "not available" for invalid fields; decoders reject frames of the
 * wrong PGN or length and report "not available" / error values as invalid.
 *
 * Layouts are original work from public sources. Verify them against your
 * copy of SAE J1939-71. Byte indices below are 0-based (J1939 documents
 * count from 1).
 */

#ifndef OPEN_CRANK_J1939_H
#define OPEN_CRANK_J1939_H

#include <stdbool.h>
#include <stdint.h>

/** @brief One CAN frame with a 29-bit identifier. */
typedef struct {
    uint32_t id;      /**< 29-bit identifier (no EFF flag) */
    uint8_t dlc;      /**< number of data bytes, 0..8 */
    uint8_t data[8];
} J1939Frame;

#define J1939_PRIORITY_CONTROL 3U
#define J1939_PRIORITY_INFO 6U

#define J1939_ADDR_ENGINE_1 0x00U
#define J1939_ADDR_NULL 0xFEU
#define J1939_ADDR_GLOBAL 0xFFU

#define J1939_PGN_TSC1 0x00000U    /**< Torque/Speed Control 1 (PGN 0) */
#define J1939_PGN_EEC1 0x0F004U    /**< Electronic Engine Controller 1 (PGN 61444) */
#define J1939_PGN_ET1 0x0FEEEU     /**< Engine Temperature 1 (PGN 65262) */
#define J1939_PGN_EFLP1 0x0FEEFU   /**< Engine Fluid Level/Pressure 1 (PGN 65263) */

/** @name Identifier helpers
 * PDU1 PGNs (PF < 240) carry a destination address in PS; PDU2 PGNs are broadcast.
 * @{ */
uint32_t j1939_make_id(uint8_t priority, uint32_t pgn, uint8_t dest, uint8_t src);
uint32_t j1939_id_pgn(uint32_t id);       /**< PGN with the destination stripped for PDU1 */
uint8_t j1939_id_source(uint32_t id);
uint8_t j1939_id_dest(uint32_t id);       /**< J1939_ADDR_GLOBAL for PDU2 */
uint8_t j1939_id_priority(uint32_t id);
/** @} */

/** @brief Override control modes (SPN 695). */
typedef enum {
    J1939_OVERRIDE_DISABLED = 0,
    J1939_OVERRIDE_SPEED = 1,
    J1939_OVERRIDE_TORQUE = 2,
    J1939_OVERRIDE_SPEED_TORQUE_LIMIT = 3
} J1939OverrideMode;

/**
 * @brief TSC1, PGN 0, destination-specific. Byte 0: SPN 695 (bits 0-1),
 *        SPN 696 (bits 2-3), SPN 897 (bits 4-5). Bytes 1-2: SPN 898,
 *        0.125 rpm/bit. Byte 3: SPN 518, 1 %/bit, offset -125. Bytes 4-6
 *        sent as 0xFF. Byte 7: message counter (SPN 4206) in bits 0-3 and
 *        message checksum (SPN 4207) in bits 4-7, as J1939-71 defines them;
 *        see j1939_tsc1_checksum().
 */
typedef struct {
    J1939OverrideMode override_mode;
    uint8_t speed_conditions;     /**< SPN 696, 0..3 */
    uint8_t mode_priority;        /**< SPN 897, 0 = highest .. 3 = lowest */
    bool requested_speed_valid;
    float requested_speed_rpm;    /**< SPN 898 */
    bool requested_torque_valid;
    float requested_torque_pct;   /**< SPN 518 */
    uint8_t message_counter;      /**< SPN 4206, 0..15: the sender adds 1 per frame, 15 wraps to 0 */
    bool checksum_valid;          /**< decode only: SPN 4207 matches the frame; ignored by the encoder */
} J1939Tsc1;

/**
 * @brief TSC1 message checksum, SPN 4207 (J1939-71): the sum of data bytes
 *        0-6, the message counter and the four bytes of the 29-bit identifier,
 *        folded to 4 bits as ((sum >> 4) + sum) & 0x0F.
 * @param f       the frame; its identifier and data bytes 0-6 are read
 * @param counter the message counter, SPN 4206 (only bits 0-3 count)
 */
uint8_t j1939_tsc1_checksum(const J1939Frame *f, uint8_t counter);

/**
 * @brief EEC1, PGN 61444, broadcast. Byte 0 bits 0-3: SPN 899. Byte 1: SPN 512.
 *        Byte 2: SPN 513 (both 1 %/bit, offset -125). Bytes 3-4: SPN 190,
 *        0.125 rpm/bit. Byte 5: SPN 1483. Bytes 6-7 and unused bits: 0xFF.
 */
typedef struct {
    uint8_t torque_mode;             /**< SPN 899, 0..15 (15 = not available) */
    bool driver_demand_torque_valid;
    float driver_demand_torque_pct;  /**< SPN 512 */
    bool actual_torque_valid;
    float actual_torque_pct;         /**< SPN 513 */
    bool engine_speed_valid;
    float engine_speed_rpm;          /**< SPN 190 */
    uint8_t controlling_address;     /**< SPN 1483, 0xFF = not available */
} J1939Eec1;

/** @brief ET1, PGN 65262. Byte 0: SPN 110, 1 degC/bit, offset -40. */
typedef struct {
    bool coolant_temp_valid;
    float coolant_temp_c;
} J1939Et1;

/** @brief EFL/P1, PGN 65263. Byte 3: SPN 100, 4 kPa/bit. */
typedef struct {
    bool oil_pressure_valid;
    float oil_pressure_kpa;
} J1939Eflp1;

/** @brief SPN 899 value "low idle governor / no request (default mode)". */
#define J1939_TORQUE_MODE_LOW_IDLE 0U
/** @brief SPN 899 value "not available". */
#define J1939_TORQUE_MODE_NA 0x0FU

void j1939_encode_tsc1(const J1939Tsc1 *m, uint8_t src, uint8_t dest, J1939Frame *f);
bool j1939_decode_tsc1(const J1939Frame *f, J1939Tsc1 *m);

void j1939_encode_eec1(const J1939Eec1 *m, uint8_t src, J1939Frame *f);
bool j1939_decode_eec1(const J1939Frame *f, J1939Eec1 *m);

void j1939_encode_et1(const J1939Et1 *m, uint8_t src, J1939Frame *f);
bool j1939_decode_et1(const J1939Frame *f, J1939Et1 *m);

void j1939_encode_eflp1(const J1939Eflp1 *m, uint8_t src, J1939Frame *f);
bool j1939_decode_eflp1(const J1939Frame *f, J1939Eflp1 *m);

#endif /* OPEN_CRANK_J1939_H */
