/**
 * @file j1939.h
 * @brief SAE J1939 CAN bus message definitions, encoders, and decoders for engine telemetry and control.
 */

#ifndef OPEN_CRANK_J1939_H
#define OPEN_CRANK_J1939_H

#include <stdint.h>
#include <stdbool.h>
#include <opencrank/plant.h>

/**
 * @name Standard 29-bit J1939 Extended CAN Identifiers
 * @brief Precomputed 29-bit identifiers combining Priority, PGN, and Source Address.
 * @{
 */
#define J1939_ID_TSC1   0x0C000003U  /**< Torque/Speed Control 1 (PGN 0, incoming from ECU/controller) */
#define J1939_ID_EEC1   0x0CF00400U  /**< Electronic Engine Controller 1 (PGN 61444, broadcasts engine RPM) */
#define J1939_ID_ET1    0x18FEEE00U  /**< Engine Temperature 1 (PGN 65262, broadcasts coolant temperature) */
#define J1939_ID_EFLP1  0x18FEEF00U  /**< Engine Fluid Level/Pressure 1 (PGN 65263, broadcasts oil pressure) */
/** @} */

/**
 * @brief Decodes an incoming J1939 CAN frame from the ECU into engine plant control inputs.
 * 
 * @param can_id 29-bit extended CAN identifier of the received frame.
 * @param data 8-byte CAN payload array.
 * @param dlc Data Length Code (number of valid bytes in the payload).
 * @param inputs Pointer to the PlantInputs structure to update with decoded commands.
 * @return True if the CAN ID was recognized and successfully decoded; false otherwise.
 */
bool j1939_decode_rx(uint32_t can_id, const uint8_t data[8], uint8_t dlc, PlantInputs *inputs);

/**
 * @brief Encodes the Electronic Engine Controller 1 (EEC1) message from the current plant state.
 * 
 * @param state Pointer to the current engine PlantState.
 * @param out_id Pointer to store the resulting 29-bit J1939 CAN identifier.
 * @param out_data 8-byte buffer where the encoded CAN payload will be written.
 */
void j1939_encode_eec1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]);

/**
 * @brief Encodes the Engine Temperature 1 (ET1) message from the current plant state.
 * 
 * @param state Pointer to the current engine PlantState.
 * @param out_id Pointer to store the resulting 29-bit J1939 CAN identifier.
 * @param out_data 8-byte buffer where the encoded CAN payload will be written.
 */
void j1939_encode_et1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]);

/**
 * @brief Encodes the Engine Fluid Level/Pressure 1 (EFLP1) message from the current plant state.
 * 
 * @param state Pointer to the current engine PlantState.
 * @param out_id Pointer to store the resulting 29-bit J1939 CAN identifier.
 * @param out_data 8-byte buffer where the encoded CAN payload will be written.
 */
void j1939_encode_eflp1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]);

#endif /* OPEN_CRANK_J1939_H */
