/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file loop.c
 * @brief Closed loop in one process. See loop.h.
 */

#include <opencrank/runtime/loop.h>

#include <string.h>

#include <opencrank/can_otk.h>

void oc_loop_init(OcLoop *l, const OcBenchConfig *cfg, const McParams *mc, const OcScenario *scenario) {
    memset(l, 0, sizeof(*l));
    oc_bench_init(&l->bench, cfg);
    oc_bench_set_scenario(&l->bench, scenario);
    mc_init(&l->mc, mc);
    otk_can_queue_init(&l->to_mc, OC_LOOP_QUEUE);
    otk_can_queue_init(&l->to_bench, OC_LOOP_QUEUE);
    l->mc_period_ms = 10U;
}

void oc_loop_step(OcLoop *l) {
    /* Controller -> bench: frames sent during the previous step. */
    J1939Frame f;
    for (size_t i = 0; i < otk_can_queue_count(&l->to_bench); i++) {
        if (oc_frame_from_otk(otk_can_queue_at(&l->to_bench, i), &f)) {
            oc_bench_rx(&l->bench, &f);
        }
    }
    otk_can_queue_clear(&l->to_bench);

    /* The machine moves on and reports. */
    J1939Frame tx[OC_LOOP_QUEUE];
    const size_t n = oc_bench_step(&l->bench, tx, OC_LOOP_QUEUE);
    for (size_t i = 0; i < n; i++) {
        OtkCanFrame o;
        oc_frame_to_otk(&tx[i], &o);
        if (!otk_can_queue_push(&l->to_mc, &o)) {
            l->queue_overflows++;
        }
    }

    /* The controller runs on its own period. */
    const uint32_t now = l->bench.t_ms;
    if (l->mc_period_ms != 0U && (now % l->mc_period_ms) == 0U) {
        for (size_t i = 0; i < otk_can_queue_count(&l->to_mc); i++) {
            if (oc_frame_from_otk(otk_can_queue_at(&l->to_mc, i), &f)) {
                mc_handle_frame(&l->mc, &f, now);
            }
        }
        otk_can_queue_clear(&l->to_mc);
        J1939Frame out[OC_LOOP_QUEUE];
        const size_t n_out = mc_step(&l->mc, now, out, OC_LOOP_QUEUE);
        for (size_t i = 0; i < n_out; i++) {
            OtkCanFrame o;
            oc_frame_to_otk(&out[i], &o);
            if (!otk_can_queue_push(&l->to_bench, &o)) {
                l->queue_overflows++;
            }
        }
    }
}
