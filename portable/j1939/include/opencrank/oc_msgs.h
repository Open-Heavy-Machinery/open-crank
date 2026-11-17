/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oc_msgs.h
 * @brief open-crank bench messages. NOT part of SAE J1939.
 *
 * In Level 1 the device under test only sees the bus, so everything the
 * machine controller exchanges with the simulated machine travels as CAN
 * frames, the same way on a laptop, in an emulator and on a real board:
 *
 * - OC_OPERATOR: the operator's controls (hand throttle, hoist lever,
 *   fault-reset button), sent by the bench. Stands in for a CAN joystick
 *   or operator panel.
 * - OC_PUMP_CMD: the hoist pump displacement command, sent by the machine
 *   controller. Stands in for the pump's proportional solenoid output.
 *
 * Both use J1939 framing in the Proprietary B range (PDU2, broadcast).
 * Scaling follows J1939 conventions; unused bytes are 0xFF.
 */

#ifndef OPEN_CRANK_OC_MSGS_H
#define OPEN_CRANK_OC_MSGS_H

#include <stdbool.h>
#include <stdint.h>

#include <opencrank/j1939.h>

#define OC_PGN_OPERATOR 0x0FF20UL  /**< Proprietary B, group extension 0x20 */
#define OC_PGN_PUMP_CMD 0x0FF21UL  /**< Proprietary B, group extension 0x21 */

#define OC_ADDR_MACHINE_CTRL 0x27U /**< default address of the machine controller */
#define OC_ADDR_BENCH 0xFCU        /**< bench (operator panel, environment) */

/**
 * @brief Operator controls. Bytes 0-1: target engine speed, 0.125 rpm/bit.
 *        Byte 2: hoist lever, 0.4 %/bit. Byte 3 bits 0-1: fault-reset button
 *        (00 released, 01 pressed, 11 not available).
 */
typedef struct {
    bool target_speed_valid;
    float target_speed_rpm;
    bool hoist_demand_valid;
    float hoist_demand_pct;
    bool fault_reset_pressed;
} OcOperator;

/** @brief Pump displacement command. Byte 0: 0.4 %/bit. */
typedef struct {
    bool displacement_valid;
    float displacement_pct;
} OcPumpCmd;

void oc_encode_operator(const OcOperator *m, uint8_t src, J1939Frame *f);
bool oc_decode_operator(const J1939Frame *f, OcOperator *m);

void oc_encode_pump_cmd(const OcPumpCmd *m, uint8_t src, J1939Frame *f);
bool oc_decode_pump_cmd(const J1939Frame *f, OcPumpCmd *m);

#endif /* OPEN_CRANK_OC_MSGS_H */
