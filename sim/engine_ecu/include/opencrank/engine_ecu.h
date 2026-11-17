/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file engine_ecu.h
 * @brief Simulated engine ECU: the engine-side brain in Level 1.
 *
 * It decides; the plant only responds. Responsibilities:
 * - starter control (cranking while START is held, released once running)
 * - no fuel below the speed at which the ECU can synchronise
 * - low-idle governor, and a PI speed governor driven by external speed
 *   requests (TSC1 speed control), clamped to the governed range
 * - optional droop (0 = isochronous)
 * - stale requests are ignored: after a timeout the engine returns to idle
 * - overspeed trip, latched until the engine has stopped
 * - stall detection
 *
 * The ECU keeps its own millisecond time, advanced only by engine_ecu_step().
 * It never reads a wall clock. Protocol decoding happens outside: this module
 * knows nothing about J1939 frames.
 */

#ifndef OPEN_CRANK_ENGINE_ECU_H
#define OPEN_CRANK_ENGINE_ECU_H

#include <stdbool.h>
#include <stdint.h>

/** @brief ECU calibration. Provenance: docs/parameters.md. */
typedef struct {
    double low_idle_rpm;          /**< idle setpoint, and the lowest accepted speed request */
    double high_idle_rpm;         /**< governed ceiling: highest accepted speed request */
    double overspeed_trip_rpm;    /**< fuel is cut and latched above this speed */
    double fuel_enable_rpm;       /**< no fuel below this speed (no crank/cam sync yet) */
    double running_rpm;           /**< cranking ends and the governor takes over above this speed */
    double stall_rpm;             /**< a running engine below this speed is declared stalled */
    double start_fuel_pct;        /**< fuel while cranking */
    double max_crank_s;           /**< cranking is abandoned after this time */
    double kp_pct_per_rpm;        /**< governor proportional gain */
    double ki_pct_per_rpm_s;      /**< governor integral gain */
    double droop_pct;             /**< speed droop at full fuel, percent of setpoint (0 = isochronous) */
    uint32_t request_timeout_ms;  /**< external speed requests older than this are ignored */
    uint8_t source_address;       /**< this ECU's J1939 source address */
} EngineEcuParams;

/** @brief Override modes an external request can ask for (J1939 SPN 695 meaning). */
typedef enum {
    ECU_OVERRIDE_DISABLED = 0,
    ECU_OVERRIDE_SPEED = 1,
    ECU_OVERRIDE_TORQUE = 2,             /* not implemented: treated as disabled */
    ECU_OVERRIDE_SPEED_TORQUE_LIMIT = 3  /* not implemented: treated as disabled */
} EngineEcuOverride;

/** @brief One external speed request, already decoded from the bus. */
typedef struct {
    EngineEcuOverride mode;
    bool speed_valid;
    double speed_rpm;
    uint8_t source_address;  /**< who sent it */
} EngineEcuRequest;

/** @brief Inputs sampled every step. */
typedef struct {
    bool key_on;   /**< key switch: ECU powered */
    bool start;    /**< key held in START */
    double rpm;    /**< measured crankshaft speed */
} EngineEcuInputs;

/** @brief Outputs to the plant. */
typedef struct {
    double fuel_pct;
    bool starter_engaged;
} EngineEcuOutputs;

typedef enum {
    ECU_STATE_OFF = 0,     /**< key off */
    ECU_STATE_STOPPED,     /**< key on, engine not running */
    ECU_STATE_CRANKING,    /**< starter engaged */
    ECU_STATE_RUNNING      /**< governor in control */
} EngineEcuState;

typedef enum {
    ECU_SPEED_SOURCE_LOW_IDLE = 0,  /**< the ECU's own idle governor */
    ECU_SPEED_SOURCE_REQUEST        /**< an external speed request is in control */
} EngineEcuSpeedSource;

/** @brief ECU runtime state. Read-only for callers except through the functions below. */
typedef struct {
    EngineEcuState state;
    EngineEcuSpeedSource source;
    bool overspeed_tripped;
    bool starter_engaged;
    double fuel_pct;            /**< last fuel output */
    double target_rpm;          /**< setpoint before droop */
    double integrator_pct;
    double crank_time_s;
    uint8_t controlling_address;/**< source address of the device controlling speed (J1939 SPN 1483) */

    uint32_t t_ms;              /**< ECU time, advanced by engine_ecu_step() */
    EngineEcuRequest request;
    bool request_seen;
    uint32_t request_t_ms;

    bool ignore_requests;       /**< fault injection: the ECU ignores external requests */
} EngineEcu;

void engine_ecu_init(EngineEcu *e, const EngineEcuParams *p);

/** @brief Hands the ECU a request received now (stamped with the ECU's own time). */
void engine_ecu_request(EngineEcu *e, const EngineEcuRequest *req);

/** @brief Advances the ECU by @p dt_ms and computes its outputs. */
void engine_ecu_step(EngineEcu *e, const EngineEcuParams *p, const EngineEcuInputs *in, uint32_t dt_ms,
                     EngineEcuOutputs *out);

#endif /* OPEN_CRANK_ENGINE_ECU_H */
