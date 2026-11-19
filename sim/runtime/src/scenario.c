/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file scenario.c
 * @brief open-tick's scenario parser bound to open-crank's signals. See scenario.h.
 */

#include <opencrank/runtime/scenario.h>

bool oc_scenario_parse(const char *text, OcScenario *s, char *err, size_t err_len) {
    return otk_scenario_parse(text, &OC_SIGNAL_TABLE, s, err, err_len);
}

bool oc_scenario_load(const char *path, OcScenario *s, char *err, size_t err_len) {
    return otk_scenario_load(path, &OC_SIGNAL_TABLE, s, err, err_len);
}
