/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file loop.c
 * @brief Closed loop in one process. See loop.h.
 */

#include <opencrank/runtime/loop.h>

#include <string.h>

void oc_loop_init(OcLoop *l, const OcBenchConfig *cfg, const McParams *mc, const OcScenario *scenario) {
    memset(l, 0, sizeof(*l));
    oc_bench_init(&l->bench, cfg);
    oc_bench_set_scenario(&l->bench, scenario);
    mc_init(&l->mc, mc);
    l->mc_period_ms = 10U;
}

void oc_loop_step(OcLoop *l) {
    /* Controller -> bench: frames sent during the previous step. */
    for (size_t i = 0; i < l->n_to_bench; i++) {
        oc_bench_rx(&l->bench, &l->to_bench[i]);
    }
    l->n_to_bench = 0U;

    /* The machine moves on and reports. */
    J1939Frame tx[OC_LOOP_QUEUE];
    const size_t n = oc_bench_step(&l->bench, tx, OC_LOOP_QUEUE);
    for (size_t i = 0; i < n; i++) {
        if (l->n_to_mc < OC_LOOP_QUEUE) {
            l->to_mc[l->n_to_mc++] = tx[i];
        } else {
            l->queue_overflows++;
        }
    }

    /* The controller runs on its own period. */
    const uint32_t now = l->bench.t_ms;
    if (l->mc_period_ms != 0U && (now % l->mc_period_ms) == 0U) {
        for (size_t i = 0; i < l->n_to_mc; i++) {
            mc_handle_frame(&l->mc, &l->to_mc[i], now);
        }
        l->n_to_mc = 0U;
        l->n_to_bench = mc_step(&l->mc, now, l->to_bench, OC_LOOP_QUEUE);
    }
}
