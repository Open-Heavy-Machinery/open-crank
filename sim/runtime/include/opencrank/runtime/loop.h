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
 */

#ifndef OPEN_CRANK_RUNTIME_LOOP_H
#define OPEN_CRANK_RUNTIME_LOOP_H

#include <stddef.h>
#include <stdint.h>

#include <opencrank/machine_ctrl.h>
#include <opencrank/runtime/bench.h>

#define OC_LOOP_QUEUE 32U

typedef struct {
    OcBench bench;
    McCtx mc;
    uint32_t mc_period_ms;

    J1939Frame to_mc[OC_LOOP_QUEUE];
    size_t n_to_mc;
    J1939Frame to_bench[OC_LOOP_QUEUE];
    size_t n_to_bench;
    uint32_t queue_overflows;
} OcLoop;

/**
 * @brief Sets up bench and controller.
 * @param scenario optional; may be NULL
 */
void oc_loop_init(OcLoop *l, const OcBenchConfig *cfg, const McParams *mc, const OcScenario *scenario);

/** @brief Advances the loop by one bench step. */
void oc_loop_step(OcLoop *l);

#endif /* OPEN_CRANK_RUNTIME_LOOP_H */
