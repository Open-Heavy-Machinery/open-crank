/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file ecu.h
 * @brief Reference engine-ECU logic for Level 2 (v0.4). Portable: no I/O, no
 *        clock, no allocation, float only. The board's firmware and the
 *        in-memory Level 2 loop run this same code.
 *
 * Two parts, with different owners (docs/porting.md):
 *
 * - The wheel (EcuWheel), in the context of the crank and cam capture
 *   interrupt: ecu_wheel_tooth() on every rising crank edge, ecu_wheel_cam()
 *   on every rising cam edge, ecu_wheel_poll() at least ten times a second
 *   in the same context. Times are ticks of a free-running 32-bit timer
 *   (EcuParams.tick_hz). It finds the 60-2 gap, counts the teeth, takes the
 *   cycle's phase from the cam, measures the speed, and says which injector
 *   to arm, with its delay from the next tooth's edge and its width.
 *
 *   Synchronisation: NONE -> SEEKING (edges arrive) -> CRANK (a gap, then a
 *   second gap exactly 58 teeth later: 360 degrees known) -> FULL (the cam
 *   edge in its window: 720 degrees known). A cam edge in its window while
 *   the teeth are counted towards the second gap gives FULL at that gap, so
 *   FULL comes within three revolutions of the first edge from any angle.
 *   A gap is a tooth period over 1.5
 *   times the one before, confirmed when the next is under half of it. Sync
 *   is lost (back to SEEKING, sync_losses + 1) on a gap where none belongs,
 *   no gap where one belongs, or a cam missing or out of its window two
 *   cycles in a row. No tooth for no_tooth_ms is a stop: NONE, stops + 1.
 *
 * - The governor (EcuCtx), in the main loop: ecu_handle_frame() for every
 *   received frame, ecu_step() every 10 ms. Level 1's PI speed governor (the
 *   simulated ECU's law and anti-windup), low idle or a TSC1 speed request,
 *   start fuel until running, an overspeed trip latched until the engine
 *   stops, no fuel outside FULL sync. It sends EEC1.
 *
 * The hand-over between the two contexts is the platform's:
 * - wheel -> governor: an EcuWheelStatus, copied by ecu_wheel_status() with
 *   the capture interrupt masked;
 * - governor -> wheel: the commanded injector width in ticks, one uint32_t
 *   (ecu_width_ticks() then ecu_wheel_set_width()), a single aligned store.
 *
 * Angles are crank degrees of the 720-degree cycle, 0 at the compression TDC
 * of cylinder 1, as the Level 2 plant (sim/l2) and the bridge's wheel are
 * configured. Injector k is cylinder k.
 */

#ifndef OPEN_CRANK_ECU_H
#define OPEN_CRANK_ECU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/j1939.h>

/** @brief Fixed by the 60-2 wheel and the six-cylinder engine. */
enum {
    ECU_TEETH = 58,      /**< real teeth per revolution (60 - 2) */
    ECU_CYLINDERS = 6,
    ECU_SPEED_RING = 24  /**< edges kept for the speed over 20 tooth positions */
};

/** @brief ECU configuration. Provenance: docs/parameters.md. */
typedef struct {
    uint32_t tick_hz;          /**< the capture timer's frequency (170 MHz on the reference board) */
    float tooth0_deg;          /**< where the first tooth after the gap begins (-60: TDC 1 is tooth 10) */
    float cam_deg;             /**< where the cam's rising edge is, in the first revolution (90) */
    float cam_window_deg;      /**< the cam edge is accepted this far either side (30) */
    float tdc_deg[ECU_CYLINDERS]; /**< compression TDC of cylinders 1-6 */
    float soi_btdc_deg;        /**< start of injection, degrees before TDC */
    float injector_dead_us;    /**< injector model: width = dead + full * fuel / 100 */
    float injector_full_us;
    uint32_t no_tooth_ms;      /**< no tooth for this long is a stop */

    float low_idle_rpm;
    float high_idle_rpm;
    float overspeed_trip_rpm;  /**< fuel cut, latched until the engine stops */
    float running_rpm;         /**< start fuel ends and the governor takes over */
    float start_fuel_pct;
    float kp_pct_per_rpm;
    float ki_pct_per_rpm_s;
    uint32_t request_timeout_ms; /**< a TSC1 older than this is ignored: back to low idle */
    uint8_t source_address;
} EcuParams;

/** @brief The d936la6 engine on the 60-2 wheel of the Level 2 plant, with the reference board's timer. */
extern const EcuParams ECU_DEFAULT_PARAMS;

typedef enum {
    ECU_SYNC_NONE = 0, /**< no edge, or stopped */
    ECU_SYNC_SEEKING,  /**< edges arrive; no gap confirmed twice 58 teeth apart yet */
    ECU_SYNC_CRANK,    /**< 360 degrees known */
    ECU_SYNC_FULL      /**< 720 degrees known: injection allowed */
} EcuSync;

/** @brief Where each cylinder's injection is armed, computed by ecu_wheel_init(). */
typedef struct {
    uint8_t arm_rev;    /**< revolution (0: the cam's) of the edge that arms it */
    uint8_t arm_tooth;  /**< tooth whose edge arms it: the one before the reference tooth */
    float delay_deg;    /**< from the reference tooth's edge to the start of injection */
} EcuArmPoint;

/** @brief What to arm: the timer of @p injector starts on the next tooth's edge. */
typedef struct {
    uint8_t injector;     /**< 1-6 */
    uint32_t delay_ticks; /**< from the next tooth's edge to the start of injection */
    uint32_t width_ticks;
} EcuArm;

/** @brief The wheel's state, for the governor and for reports. */
typedef struct {
    EcuSync sync;
    float rpm;            /**< 0 until two edges */
    uint8_t tooth;        /**< index of the last edge, 0-57, when synchronised */
    uint8_t rev;          /**< 0 or 1, in FULL */
    uint32_t teeth;       /**< crank edges seen */
    uint32_t cam_edges;
    uint32_t gaps;        /**< gaps confirmed */
    uint32_t sync_losses;
    uint32_t stops;
    uint32_t arms;        /**< injections armed */
} EcuWheelStatus;

/** @brief Wheel context. Owned by the capture interrupt; read through ecu_wheel_status(). */
typedef struct {
    const EcuParams *p;
    EcuArmPoint arm[ECU_CYLINDERS];
    EcuWheelStatus st;

    bool have_edge;
    uint32_t last_t;
    uint32_t last_dt;     /**< the last tooth period (0 until two edges) */
    bool gap_pending;     /**< the last period looked like the gap; confirmed by the next */
    uint32_t gap_dt;
    bool counting;        /**< a gap was confirmed: tooth indices are known */
    uint8_t idx;          /**< index of the last edge, 0-57, while counting */

    bool cam_in_window;   /**< this cycle: the cam was seen in its window, in revolution 0 */
    bool cam_wrong;       /**< this cycle: a cam edge outside its window, or a second one */
    uint8_t cam_errors;   /**< consecutive cycles with a cam error */

    uint32_t ring_t[ECU_SPEED_RING];
    uint32_t ring_pos[ECU_SPEED_RING];
    uint8_t ring_head;
    uint8_t ring_n;
    uint32_t pos;         /**< tooth positions since the first edge; the gap counts 3 */

    uint32_t width_ticks; /**< commanded by the governor */
} EcuWheel;

void ecu_wheel_init(EcuWheel *w, const EcuParams *p);

/**
 * @brief A rising crank edge at @p t.
 * @return true and @p arm filled when the injector of a cylinder is to be
 *         armed now, to start on the next edge.
 */
bool ecu_wheel_tooth(EcuWheel *w, uint32_t t, EcuArm *arm);

/** @brief A rising cam edge at @p t. */
void ecu_wheel_cam(EcuWheel *w, uint32_t t);

/** @brief Detects a stop (no tooth for no_tooth_ms); call at least every 100 ms with the timer's time. */
void ecu_wheel_poll(EcuWheel *w, uint32_t now);

/** @brief The governor's commanded width, handed over by the platform. */
void ecu_wheel_set_width(EcuWheel *w, uint32_t width_ticks);

/** @brief Copies the status; the platform masks the capture interrupt around it. */
void ecu_wheel_status(const EcuWheel *w, EcuWheelStatus *out);

typedef enum {
    ECU_GOV_STOPPED = 0, /**< not in FULL sync: no fuel */
    ECU_GOV_CRANKING,    /**< FULL sync below running speed: start fuel */
    ECU_GOV_RUNNING      /**< the governor in control */
} EcuGovState;

/** @brief Governor context. Owned by the main loop. */
typedef struct {
    const EcuParams *p;
    EcuGovState state;
    bool overspeed_tripped;
    bool stepped;
    uint32_t last_step_ms;

    bool request_seen;
    uint32_t request_ms;
    bool request_speed;        /**< the last TSC1 asked for speed control with a valid speed */
    float request_rpm;
    uint8_t request_address;
    bool counter_seen;
    uint8_t counter;           /**< the last accepted TSC1's message counter */
    uint32_t tsc1_rejected;    /**< bad checksum or repeated counter */

    bool from_request;         /**< the setpoint is a TSC1's, not low idle */
    float target_rpm;
    float integrator_pct;
    float fuel_pct;
    float rpm;                 /**< the wheel's, at the last step */
    uint8_t controlling_address;
    uint32_t width_ticks;
} EcuCtx;

void ecu_init(EcuCtx *e, const EcuParams *p);

/** @brief Feeds one received frame: a TSC1 to this ECU or global. Others are ignored. */
void ecu_handle_frame(EcuCtx *e, const J1939Frame *f, uint32_t now_ms);

/**
 * @brief Runs one governor period (every 10 ms) on the wheel's @p ws.
 * @return frames written to @p out: one EEC1 if @p cap allows.
 */
size_t ecu_step(EcuCtx *e, const EcuWheelStatus *ws, uint32_t now_ms, J1939Frame *out, size_t cap);

/** @brief The injector width to hand to the wheel, in ticks; 0 = no injection. */
uint32_t ecu_width_ticks(const EcuCtx *e);

#endif /* OPEN_CRANK_ECU_H */
