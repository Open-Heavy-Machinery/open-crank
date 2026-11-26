/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file loop.h
 * @brief Closed loop in one process: the bench and the reference machine
 *        controller joined by an in-memory bus.
 *
 * Every frame goes through the real encoders and decoders; only the socket
 * is missing. Frames the bench sends in a step reach the controller in the
 * same step; frames the controller sends reach the bench at the start of the
 * next step. Deterministic and free-running: as fast as the CPU allows.
 *
 * With oc_loop_set_frame_log(), every frame of the in-memory bus goes into an
 * open-tick frame log (framelog.h), with bus_clock=sim: a frame's bus time is
 * when the controller sent it or received it, in simulated time.
 */

#ifndef OPEN_CRANK_RUNTIME_LOOP_H
#define OPEN_CRANK_RUNTIME_LOOP_H

#include <stddef.h>
#include <stdint.h>

#include <opencrank/machine_ctrl.h>
#include <opencrank/runtime/bench.h>
#include <opentick/can.h>

#define OC_LOOP_QUEUE 32U

typedef struct {
    OcBench bench;
    McCtx mc;
    uint32_t mc_period_ms;

    OtkCanQueue to_mc;
    OtkCanQueue to_bench;
    uint32_t queue_overflows;

    OtkFrameLog *frames;                   /**< optional frame log */
    OcBenchTxInfo to_mc_info[OC_LOOP_QUEUE]; /**< what the log needs about each frame in to_mc */
    uint32_t mc_sent_ms;                   /**< when the controller sent what is in to_bench */
} OcLoop;

/**
 * @brief Sets up bench and controller.
 * @param scenario optional; may be NULL
 */
void oc_loop_init(OcLoop *l, const OcBenchConfig *cfg, const McParams *mc, const OcScenario *scenario);

/** @brief Advances the loop by one bench step. */
void oc_loop_step(OcLoop *l);

/**
 * @brief Writes every frame of the in-memory bus into @p log (step 10 ms,
 *        bus_clock=sim), or stops if NULL. Set it before the first step.
 */
void oc_loop_set_frame_log(OcLoop *l, OtkFrameLog *log);

#endif /* OPEN_CRANK_RUNTIME_LOOP_H */
