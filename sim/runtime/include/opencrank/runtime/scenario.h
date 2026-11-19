/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file scenario.h
 * @brief Scenarios: open-tick's CSV scenarios, with open-crank's signals.
 *
 * The parser lives in open-tick (opentick/scenario.h); these wrappers bind
 * it to OC_SIGNAL_TABLE. Event times are nanoseconds; `end` is reserved by
 * open-tick and stored with the id OTK_SIGNAL_END.
 */

#ifndef OPEN_CRANK_RUNTIME_SCENARIO_H
#define OPEN_CRANK_RUNTIME_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>

#include <opencrank/runtime/signals.h>
#include <opentick/scenario.h>

typedef OtkScenario OcScenario;

bool oc_scenario_parse(const char *text, OcScenario *s, char *err, size_t err_len);
bool oc_scenario_load(const char *path, OcScenario *s, char *err, size_t err_len);

#endif /* OPEN_CRANK_RUNTIME_SCENARIO_H */
