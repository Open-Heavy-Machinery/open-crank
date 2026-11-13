/**
 * @file test_d936.c
 * @brief Unit tests for the Liebherr D936L A6 engine plant startup dynamics and J1939 EEC1 encoding.
 */

#include <assert.h>
#include <stdio.h>
#include <opencrank/plant.h>
#include <opencrank/plants/d936la6.h>
#include <opencrank/j1939.h>

/**
 * @brief Executes automated verification tests for engine cranking/startup and J1939 telemetry serialization.
 * @return 0 if all assertions pass.
 */
int main(void) {
    PlantState engine;
    PlantInputs inputs = { .throttle_pct = 0.0, .load_torque_nm = 0.0, .starter_active = true };

    /* 1. Verify cold cranking and startup sequence from 0 RPM using the starter motor */
    plant_init(&engine, &PLANT_D936LA6, 0.0);
    for (int i = 0; i < 200; i++) { /* Simulate 2.0 seconds at 100 Hz (dt = 10 ms) */
        if (engine.rpm > 350.0) {
            /* Disengage starter motor and apply 25% throttle once combustion sustains rotation */
            inputs.starter_active = false;
            inputs.throttle_pct = 25.0;
        }
        plant_step(&engine, &PLANT_D936LA6, &inputs, 0.010);
    }
    assert(engine.rpm > 1000.0);

    /* 2. Verify SAE J1939 EEC1 (SPN 190 - Engine Speed) frame encoding and resolution accuracy */
    uint32_t id = 0;
    uint8_t data[8] = {0};
    j1939_encode_eec1(&engine, &id, data);
    assert(id == J1939_ID_EEC1);

    /* Reconstruct 16-bit little-endian raw RPM value and scale by 0.125 RPM/bit */
    uint16_t decoded_raw = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    double decoded_rpm = decoded_raw * 0.125;
    assert((engine.rpm - decoded_rpm) < 0.25);

    printf("ALL TESTS PASSED! Final RPM: %.1f\n", engine.rpm);
    return 0;
}
