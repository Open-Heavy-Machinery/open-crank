/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file trace.h
 * @brief CSV trace of a run: one row per bench step, readable by any
 *        spreadsheet or by pandas. Controller columns are empty when the
 *        controller runs in another process (SIL over SocketCAN).
 */

#ifndef OPEN_CRANK_RUNTIME_TRACE_H
#define OPEN_CRANK_RUNTIME_TRACE_H

#include <stdio.h>

#include <opencrank/machine_ctrl.h>
#include <opencrank/runtime/bench.h>

void oc_trace_header(FILE *fp);

/** @param mc may be NULL */
void oc_trace_row(FILE *fp, const OcBench *b, const McCtx *mc);

const char *oc_ecu_state_name(EngineEcuState s);

#endif /* OPEN_CRANK_RUNTIME_TRACE_H */
