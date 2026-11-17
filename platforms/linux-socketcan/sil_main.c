/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file sil_main.c
 * @brief open-crank-sil: the simulated machine (plant, engine ECU, hoist
 *        pump, operator panel) on a SocketCAN bus, paced by the wall clock.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <opencrank/runtime/bench.h>

#include "pacer.h"
#include "socketcan.h"

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

/* Operating profile (moves to a scenario file in the next step). */
static void apply_profile(OcBenchSignals *s, double t, double target_rpm) {
    s->key_on = true;
    s->start = (t >= 0.5 && t < 3.0);
    s->target_rpm = (t >= 4.0) ? target_rpm : 0.0;
    s->hoist_demand_pct = (t >= 10.0 && t < 20.0) ? 85.0 : 0.0;
    s->hoist_load_bar = 250.0;
}

int main(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "vcan0";
    const double target_rpm = (argc > 2) ? strtod(argv[2], NULL) : 1500.0;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int sock = oc_can_open(ifname);
    if (sock < 0) {
        perror("open-crank-sil: cannot open CAN interface. Try: sudo modprobe vcan && "
               "sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0");
        return 1;
    }

    OcBench bench;
    oc_bench_init(&bench, &OC_BENCH_D936LA6);
    OcPacer pacer;
    oc_pacer_init(&pacer, bench.cfg->step_ms);

    printf("open-crank-sil: [%s] on %s (Ctrl+C to stop)\n", bench.cfg->plant->name, ifname);

    while (g_running) {
        J1939Frame f;
        while (oc_can_recv(sock, &f) > 0) {
            oc_bench_rx(&bench, &f);
        }

        apply_profile(&bench.sig, oc_bench_time_s(&bench), target_rpm);

        J1939Frame tx[8];
        const size_t n = oc_bench_step(&bench, tx, sizeof(tx) / sizeof(tx[0]));
        for (size_t i = 0; i < n; i++) {
            (void)oc_can_send(sock, &tx[i]);
        }

        if (bench.t_ms % 500U == 0U) {
            printf("[%7.2fs] rpm %7.1f | fuel %5.1f %% | load %6.1f Nm | speed set by 0x%02X | pump cmd %5.1f %%\n",
                   oc_bench_time_s(&bench), bench.plant.rpm, bench.plant.fuel_pct, bench.load_torque_nm,
                   (unsigned)bench.ecu.controlling_address, bench.pump_cmd_applied_pct);
            fflush(stdout);
        }
        oc_pacer_wait(&pacer);
    }

    printf("open-crank-sil: stopped (%u late steps)\n", (unsigned)pacer.overruns);
    close(sock);
    return 0;
}
