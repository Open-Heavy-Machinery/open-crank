/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_scaling.h
 * @brief Internal helpers: saturating SPN scaling shared by the codec sources.
 *
 * One-byte parameters: 0..250 valid, 251..255 error / not available.
 * Two-byte parameters: 0..0xFAFF valid, above that error / not available.
 */

#ifndef OPEN_CRANK_J1939_SCALING_H
#define OPEN_CRANK_J1939_SCALING_H

#include <opencrank/j1939.h>

#define J1939_U8_MAX_VALID 250U
#define J1939_U16_MAX_VALID 0xFAFFU
#define J1939_U8_NA 0xFFU
#define J1939_U16_NA 0xFFFFU

/* Clamps v to [lo, hi]; NaN maps to lo. */
static inline float j1939_clampf(float v, float lo, float hi) {
    if (!(v >= lo)) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

/* Physical value -> raw, with raw = (value - offset) / resolution, rounded and saturated. */
static inline uint8_t j1939_to_u8(float value, float resolution, float offset) {
    const float raw = j1939_clampf((value - offset) / resolution, 0.0f, (float)J1939_U8_MAX_VALID);
    return (uint8_t)(raw + 0.5f);
}

static inline uint16_t j1939_to_u16(float value, float resolution, float offset) {
    const float raw = j1939_clampf((value - offset) / resolution, 0.0f, (float)J1939_U16_MAX_VALID);
    return (uint16_t)(raw + 0.5f);
}

static inline bool j1939_from_u8(uint8_t raw, float resolution, float offset, float *value) {
    if (raw > J1939_U8_MAX_VALID) {
        *value = 0.0f;
        return false;
    }
    *value = (float)raw * resolution + offset;
    return true;
}

static inline bool j1939_from_u16(uint16_t raw, float resolution, float offset, float *value) {
    if (raw > J1939_U16_MAX_VALID) {
        *value = 0.0f;
        return false;
    }
    *value = (float)raw * resolution + offset;
    return true;
}

static inline void j1939_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static inline uint16_t j1939_get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* Starts an 8-byte frame with every byte "not available". */
static inline void j1939_frame_init(J1939Frame *f, uint32_t id) {
    f->id = id;
    f->dlc = 8U;
    for (uint8_t i = 0U; i < 8U; i++) {
        f->data[i] = 0xFFU;
    }
}

static inline bool j1939_frame_is(const J1939Frame *f, uint32_t pgn) {
    return (f->dlc == 8U) && (j1939_id_pgn(f->id) == pgn);
}

#endif /* OPEN_CRANK_J1939_SCALING_H */
