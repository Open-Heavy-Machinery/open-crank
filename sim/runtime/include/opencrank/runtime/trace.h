/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file trace.h
 * @brief CSV trace of a run: one row per bench step, written through
 *        open-tick's trace writer with open-crank's columns. Controller
 *        columns are empty when the controller runs in another process.
 */

#ifndef OPEN_CRANK_RUNTIME_TRACE_H
#define OPEN_CRANK_RUNTIME_TRACE_H

#include <stdbool.h>
#include <stdio.h>

#include <opencrank/machine_ctrl.h>
#include <opencrank/runtime/bench.h>
#include <opentick/trace.h>

/** @brief Registers open-crank's columns and writes the header. */
bool oc_trace_open(OtkTrace *t, FILE *fp);

/** @param mc may be NULL */
void oc_trace_row(OtkTrace *t, const OcBench *b, const McCtx *mc);

const char *oc_ecu_state_name(EngineEcuState s);

#endif /* OPEN_CRANK_RUNTIME_TRACE_H */
