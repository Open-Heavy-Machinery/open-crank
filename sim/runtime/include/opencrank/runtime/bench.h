/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bench.h
 * @brief The simulated machine as seen from the CAN bus.
 *
 * The bench owns the plant, the simulated engine ECU and the hoist pump. It
 * decodes the frames a Level 1 device under test sends (TSC1, OC_PUMP_CMD),
 * advances the physics by one fixed step, and returns the frames the
 * simulated machine transmits (EEC1, ET1, EFL/P1, OC_OPERATOR).
 *
 * Time: the bench owns simulated time as an integer millisecond count and
 * never reads a clock. Whoever calls oc_bench_step() decides how fast time
 * passes: a free-running loop (tests, CI), a wall-clock-paced loop (SIL demo)
 * or, later, an emulator (virtual time).
 *
 * Frame log (open-tick framelog.h): with oc_bench_set_frame_log(), the bench
 * writes the rows it can complete itself: every frame from the device under
 * test (delivered or lost on the mc link, or dropped by fault.mc_tx_lost),
 * and every machine frame lost on the engine link. The frames it puts on the
 * bus (oc_bench_step_ex()) come with an OcBenchTxInfo; the runner writes
 * their rows once it knows when they were on the bus.
 */

#ifndef OPEN_CRANK_RUNTIME_BENCH_H
#define OPEN_CRANK_RUNTIME_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/engine_ecu.h>
#include <opencrank/j1939.h>
#include <opencrank/loads/hydraulic_pump.h>
#include <opencrank/plant.h>
#include <opencrank/runtime/scenario.h>
#include <opencrank/runtime/signals.h>
#include <opencrank/j1939_diag_tx.h>
#include <opentick/framelog.h>
#include <opentick/link.h>
#include <opentick/player.h>

/** @brief What the bench simulates, and how. */
/** @brief What the frame log needs about a frame the bench puts on the bus. */
typedef struct {
    OtkNs t;             /**< logical time: the step that sent it (it may have waited in the engine link) */
    const char *link;    /**< "engine", or NULL (operator panel, test frame: no link) */
    OtkFrameFault fault; /**< OTK_FRAME_OK or OTK_FRAME_CORRUPT */
} OcBenchTxInfo;

/** @brief A frame from the device under test in the mc link, with its bus time. */
typedef struct {
    OtkNs pushed;
    uint32_t id;
    OtkNs t_bus;
} OcBenchInFlight;

typedef struct {
    const PlantParams *plant;
    const EngineEcuParams *ecu;
    const HydraulicPumpParams *pump;
    uint32_t step_ms;               /**< physics step */
    uint32_t pump_cmd_timeout_ms;   /**< a stale pump command counts as zero (solenoid de-energised) */
    uint32_t eec1_period_ms;
    uint32_t eflp1_period_ms;
    uint32_t et1_period_ms;
    uint32_t operator_period_ms;
    uint32_t dm1_period_ms;         /**< DM1 is sent this often, and at once when it changes */
} OcBenchConfig;

/**
 * @brief Seeds of the links' PRNGs (jitter). Fixed, so a scenario with jitter
 *        gives the same trace on every run.
 */
#define OC_BENCH_LINK_MC_SEED 1U
#define OC_BENCH_LINK_ENGINE_SEED 2U

/** @brief The reference configuration: d936la6 plant and ECU, hoist pump. */
extern const OcBenchConfig OC_BENCH_D936LA6;

typedef struct {
    const OcBenchConfig *cfg;
    OcSignals sig;               /**< operator, environment and fault-injection inputs */
    OtkPlayer player;            /**< optional scenario: drives sig over time */

    PlantState plant;
    EngineEcu ecu;
    HydraulicPumpState pump;

    uint32_t t_ms;               /**< simulated time at the start of the next step */
    double pump_cmd_pct;         /**< last received displacement command */
    bool pump_cmd_seen;
    uint32_t pump_cmd_ms;
    double pump_cmd_applied_pct; /**< command actually applied (after timeout) */
    double load_torque_nm;       /**< total load torque applied in the last step */

    uint32_t rx_frames;          /**< frames accepted from the device under test */
    uint32_t rx_dropped;         /**< frames dropped by fault.mc_tx_lost */
    uint32_t tsc1_rejected;      /**< TSC1 refused by the ECU: wrong checksum or repeated counter (SPN 4207/4206) */
    bool tsc1_counter_seen;
    uint8_t tsc1_counter;        /**< message counter of the last TSC1 accepted */

    /**
     * Transport faults (opentick/link.h), driven by the fault.link.* signals.
     * mc: what the controller sends to the machine (TSC1, OC_PUMP_CMD), applied
     * in oc_bench_rx(). engine: what the engine sends to the controller (EEC1,
     * ET1, EFL/P1), applied in oc_bench_step(); the operator panel's frames do
     * not pass through it. No filters: a fault applies to every frame of its
     * direction. With no fault set, both are transparent.
     */
    OtkLink mc_link;
    OtkLink engine_link;

    /* The engine's DM1 (fault.engine_dtc_*): what was last sent, and a BAM in progress. */
    bool dm1_sent;
    bool dm1_red;
    bool dm1_amber;
    bool dm1_pending;                        /**< a DM1 is due but a BAM is still being sent */
    J1939Frame bam[J1939_TP_MAX_PACKETS + 1U];
    uint8_t bam_n;                           /**< frames of the BAM in progress (TP.CM included) */
    uint8_t bam_next;                        /**< next one to send */
    uint32_t bam_next_ms;                    /**< when */

    /* Frame log (optional): the bus times of the frames in the mc link. */
    OtkFrameLog *frames;
    OcBenchInFlight mc_in_flight[OTK_CAN_QUEUE_MAX];
    size_t mc_in_flight_n;
} OcBench;

void oc_bench_init(OcBench *b, const OcBenchConfig *cfg);

/** @brief Lets a scenario drive the signals. Events apply at the start of the step they fall in. */
void oc_bench_set_scenario(OcBench *b, const OcScenario *s);

/** @brief True once the scenario's `end` time is reached (never true without a scenario or `end`). */
bool oc_bench_scenario_done(const OcBench *b);

/**
 * @brief Hands the bench one frame received from the device under test. It
 *        passes through the mc link: with no fault set it applies at once.
 */
void oc_bench_rx(OcBench *b, const J1939Frame *f);

/**
 * @brief oc_bench_rx(), with the time the frame was on the bus (@p t_bus,
 *        or OTK_FRAMELOG_NO_TIME) for the frame log.
 */
void oc_bench_rx_at(OcBench *b, const J1939Frame *f, OtkNs t_bus);

/**
 * @brief Advances the simulated machine by one step.
 * @return number of frames written to @p tx (at most @p cap) to put on the bus.
 */
size_t oc_bench_step(OcBench *b, J1939Frame *tx, size_t cap);

/**
 * @brief oc_bench_step(), and for each frame in @p tx what the frame log
 *        needs (@p info, @p cap entries; may be NULL).
 */
size_t oc_bench_step_ex(OcBench *b, J1939Frame *tx, OcBenchTxInfo *info, size_t cap);

/**
 * @brief Writes the bench's rows (see above) into @p log, or stops if NULL.
 *        The log's step must be the bench's. Set it before the first step.
 */
void oc_bench_set_frame_log(OcBench *b, OtkFrameLog *log);

/**
 * @brief Frames lost because a link's line was full (opentick/link.h). Not a
 *        fault the scenario asked for: a run with any is broken.
 */
uint32_t oc_bench_link_overflows(const OcBench *b);

/** @brief Simulated time in seconds. */
double oc_bench_time_s(const OcBench *b);

#endif /* OPEN_CRANK_RUNTIME_BENCH_H */
