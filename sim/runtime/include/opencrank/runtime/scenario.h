/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file scenario.h
 * @brief Scenarios: time-ordered signal changes, written as plain CSV.
 *
 *     # comment lines and blank lines are ignored
 *     time_s,signal,value
 *     0.0,key_on,1
 *     0.5,start,1
 *     3.0,start,0
 *     8.0,hoist_demand_pct,85
 *     20.0,end,0
 *
 * Times must not decrease. Events at the same time apply in file order.
 * Unknown signals, bad numbers and out-of-order times are errors, reported
 * with their line number: a typo must fail loudly, not run a different test.
 */

#ifndef OPEN_CRANK_RUNTIME_SCENARIO_H
#define OPEN_CRANK_RUNTIME_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/runtime/signals.h>

#define OC_SCENARIO_MAX_EVENTS 256U

typedef struct {
    uint32_t t_ms;
    OcSignalId signal;
    double value;
} OcEvent;

typedef struct {
    OcEvent events[OC_SCENARIO_MAX_EVENTS];
    size_t n;
    bool has_end;
    uint32_t end_ms;  /**< time of the `end` event, if any */
} OcScenario;

/**
 * @brief Parses scenario text.
 * @param err receives a message such as "line 4: unknown signal 'kye_on'" on failure
 * @return true on success
 */
bool oc_scenario_parse(const char *text, OcScenario *s, char *err, size_t err_len);

/** @brief Reads and parses a scenario file. */
bool oc_scenario_load(const char *path, OcScenario *s, char *err, size_t err_len);

#endif /* OPEN_CRANK_RUNTIME_SCENARIO_H */
