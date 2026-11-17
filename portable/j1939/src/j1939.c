/**
 * @file j1939.c
 * @brief Implementation of SAE J1939 CAN frame encoding and decoding for engine telemetry and control.
 */

#include <opencrank/j1939.h>
#include <string.h>

bool j1939_decode_rx(uint32_t can_id, const uint8_t data[8], uint8_t dlc, PlantInputs *inputs) {
    if (can_id == J1939_ID_TSC1 && dlc >= 2) {
        /* Byte 1 (0-indexed): Requested driver/ECU demand command (0..250 raw -> 0..100%) */
        double cmd = ((double)data[1] / 250.0) * 100.0;
        if (cmd > 100.0) {
            cmd = 100.0;
        }
        inputs->throttle_pct = cmd;
        return true;
    }
    return false;
}

void j1939_encode_eec1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]) {
    /* Fill unused J1939 payload bytes with 0xFF ("Not Available" per SAE J1939-71) */
    memset(out_data, 0xFF, 8);
    *out_id = J1939_ID_EEC1;

    /* SPN 190: Engine Speed (resolution: 0.125 RPM/bit, offset: 0, bytes 3-4 little-endian) */
    uint16_t raw_rpm = (uint16_t)(state->rpm / 0.125);
    out_data[3] = (uint8_t)(raw_rpm & 0xFFU);
    out_data[4] = (uint8_t)((raw_rpm >> 8) & 0xFFU);
}

void j1939_encode_et1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]) {
    /* Fill unused J1939 payload bytes with 0xFF ("Not Available") */
    memset(out_data, 0xFF, 8);
    *out_id = J1939_ID_ET1;

    /* SPN 110: Engine Coolant Temperature (resolution: 1 °C/bit, offset: -40 °C, byte 0) */
    double temp = state->coolant_temp_c + 40.0;
    if (temp < 0.0) {
        temp = 0.0;
    }
    if (temp > 250.0) {
        temp = 250.0; /* Clamp to valid J1939 operational range (251..255 reserved for errors/N/A) */
    }
    out_data[0] = (uint8_t)temp;
}

void j1939_encode_eflp1(const PlantState *state, uint32_t *out_id, uint8_t out_data[8]) {
    /* Fill unused J1939 payload bytes with 0xFF ("Not Available") */
    memset(out_data, 0xFF, 8);
    *out_id = J1939_ID_EFLP1;

    /* SPN 100: Engine Oil Pressure (resolution: 4 kPa/bit, offset: 0, byte 3) */
    double oil = state->oil_pressure_kpa / 4.0;
    if (oil < 0.0) {
        oil = 0.0;
    }
    if (oil > 250.0) {
        oil = 250.0; /* Clamp to valid J1939 operational range (0..1000 kPa) */
    }
    out_data[3] = (uint8_t)oil;
}
