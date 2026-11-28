/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plant_l2.h
 * @brief The Level 2 plant (v0.4): the mean-value plant of sim/core with a
 *        crank angle over the 720-degree cycle, fuelled by captured injector
 *        pulses instead of a simulated ECU.
 *
 * The angle is the bridge's own (open-tick docs/bridge-protocol.md §3.2): a
 * 48-bit phase accumulator, 2^48 = 720 degrees, that gains the step's
 * `crank_phase_inc` every tick of the bridge's 27 MHz clock. The plant keeps
 * the same accumulator with the same quantised increment it sends, so its
 * angle and the bridge's are equal, tick for tick, and a capture's start tick
 * gives the exact angle at which an injection began. This holds from a freshly
 * loaded bitstream, whose accumulator is 0 at step 0 (§8, step 1).
 *
 * Angles are crank degrees, 0-720, 0 at the compression TDC of cylinder 1.
 * Firing order 1-5-3-6-2-4, one TDC every 120 degrees. Injector k is cylinder
 * k and the bridge's capture channel k - 1 (platforms/stm32g4/WIRING-L2.md).
 *
 * Fuel (decision 2 of the Level 2 design, docs/parameters.md): a linear
 * injector, width = dead + full * fuel_pct / 100. An injection counts for a
 * cylinder when its start lies inside that cylinder's window around its
 * compression TDC and it arrives on that cylinder's channel. Each cylinder
 * keeps the fuel of its last counted injection; when a window closes with no
 * counted injection, that cylinder's fuel drops to zero (a misfire). The plant
 * burns the mean of the six. An injection outside every window, or on another
 * cylinder's channel, burns nothing and is counted as misplaced.
 *
 * Captures reach the plant late: H steps of look-ahead plus a round trip on a
 * hard run (§2), a modelled delay in memory. A capture is placed by its own
 * tick, through a history of the steps already computed, so its angle does not
 * depend on when it arrives. The misfire check looks back by the same delay
 * (`capture_delay_steps`), so a window is judged only once its injection had
 * time to arrive.
 *
 * Like the plant it wraps, it has no clock, no I/O and no global state.
 */

#ifndef OPEN_CRANK_L2_PLANT_L2_H
#define OPEN_CRANK_L2_PLANT_L2_H

#include <stdbool.h>
#include <stdint.h>

#include <opencrank/plant.h>
#include <opentick/bridge.h>

/** @brief The bridge's clock and the Level 2 step. */
#define OC_L2_TICK_HZ 27000000u
#define OC_L2_STEP_TICKS 27000u /* 1 ms */
#define OC_L2_STEP_S 0.001

/** @brief The 60-2 wheel and the cam (decision 6). */
#define OC_L2_CRANK_TEETH 60u
#define OC_L2_CRANK_MISSING 2u
#define OC_L2_TOOTH0_DEG (-60.0) /* the first tooth after the gap begins 60 degrees before TDC 1 */
#define OC_L2_CAM_START_DEG 90.0
#define OC_L2_CAM_END_DEG 120.0

#define OC_L2_CYLINDERS 6u

/** @brief Steps of history kept to place late captures: 256 ms at 1 ms steps. */
#define OC_L2_HISTORY 256u

/** @brief Static parameters of the Level 2 plant. Provenance: docs/parameters.md. */
typedef struct {
    double injector_dead_us;      /**< width that delivers no fuel */
    double injector_full_us;      /**< width above the dead time for 100 % fuel */
    double window_before_deg;     /**< an injection counts from this far before its TDC */
    double window_after_deg;      /**< to this far after it */
    uint32_t capture_delay_steps; /**< the most steps a capture may arrive after it began */
} OcL2Params;

/** @brief One step already computed: where the bridge's accumulator began it, and at what increment. */
typedef struct {
    uint64_t cycle; /**< whole 720-degree cycles before this step's start */
    uint64_t acc;   /**< the 48-bit accumulator at the step's start */
    uint32_t inc;   /**< the increment of this step */
} OcL2StepRecord;

/** @brief Level 2 plant state. Read-only for callers except through the functions below. */
typedef struct {
    PlantState plant;  /**< the mean-value plant (speed, boost, temperatures, torques) */

    uint32_t step;     /**< the next step to compute; step 0 is the bridge's step 0 */
    uint64_t cycle;    /**< whole 720-degree cycles before the next step */
    uint64_t acc;      /**< the 48-bit accumulator at the next step's start */
    uint32_t inc;      /**< the increment of the last step computed (sent as crank_phase_inc) */
    OcL2StepRecord hist[OC_L2_HISTORY]; /**< step k at hist[k % OC_L2_HISTORY] */

    double cyl_fuel_pct[OC_L2_CYLINDERS];   /**< fuel of each cylinder's last counted injection */
    int64_t cyl_counted[OC_L2_CYLINDERS];   /**< the window (cycle number) of each cylinder's last counted injection */
    uint32_t checked_through;               /**< misfires are checked for steps before this one */

    uint32_t injections;   /**< counted for a cylinder */
    uint32_t misplaced;    /**< outside every window, or on another cylinder's channel */
    uint32_t misfires;     /**< windows that closed with no counted injection */
    uint32_t late_captures;/**< older than the history, or after the last step computed: ignored */
    uint32_t bad_speed;    /**< steps whose speed had no increment (should never happen) */
} OcL2Plant;

/** @brief Inputs applied during one step, besides the captured fuel. */
typedef struct {
    double load_torque_nm;
    bool starter_engaged;
} OcL2Inputs;

/** @brief Compression TDC of cylinder @p cyl (1-6), in degrees of the cycle. */
double oc_l2_tdc_deg(unsigned cyl);

/** @brief The top 32 bits of the accumulator at @p deg (any value; taken modulo 720). */
uint32_t oc_l2_phase32(double deg);

/** @brief The angle, 0 <= deg < 720, of a 48-bit accumulator value. */
double oc_l2_acc_deg(uint64_t acc);

/** @brief The bridge's configuration for this wheel and six active-high captures (§3.4). */
void oc_l2_bridge_config(OtkBridgeConfig *c);

/** @brief Injector width for @p fuel_pct (clamped to 0-100), in us. */
double oc_l2_width_us(const OcL2Params *p, double fuel_pct);

/** @brief Fuel for a pulse of @p width_us, in percent, clamped to 0-100. */
double oc_l2_fuel_pct(const OcL2Params *p, double width_us);

/** @brief The engine at rest, accumulator 0, no fuel. */
void oc_l2_init(OcL2Plant *s, const PlantParams *pp);

/**
 * @brief Computes the next step: its increment from the speed at its start,
 *        one plant_step() of 1 ms with the cylinders' mean fuel, the
 *        accumulator moved on, and the misfire check of the steps now old
 *        enough. Returns the step's increment, the setpoint's crank_phase_inc.
 */
uint32_t oc_l2_step(OcL2Plant *s, const PlantParams *pp, const OcL2Params *p, const OcL2Inputs *in);

/** @brief What a capture turned out to be. */
typedef enum {
    OC_L2_CAPTURE_COUNTED = 0, /**< fuels its cylinder */
    OC_L2_CAPTURE_MISPLACED,   /**< outside every window, or on another cylinder's channel */
    OC_L2_CAPTURE_LATE         /**< outside the history: ignored */
} OcL2CaptureResult;

/**
 * @brief One captured injector pulse. @p channel is the bridge's capture
 *        channel (0-5); @p start_tick and @p width_ticks are bridge ticks,
 *        start counted from the beginning of step 0. @p deg, if not NULL,
 *        gets the angle at which it began (0-720), when it is in the history.
 */
OcL2CaptureResult oc_l2_capture(OcL2Plant *s, const OcL2Params *p, unsigned channel, uint64_t start_tick,
                                uint32_t width_ticks, double *deg);

/**
 * @brief The crank angle at bridge tick @p tick (counted from step 0), in
 *        degrees since step 0 (not wrapped), if that tick is in a step already
 *        computed and still in the history.
 */
bool oc_l2_angle_at(const OcL2Plant *s, uint64_t tick, double *deg);

/** @brief The fuel the plant burns now: the mean of the six cylinders. */
double oc_l2_fuel_mean(const OcL2Plant *s);

#endif /* OPEN_CRANK_L2_PLANT_L2_H */
