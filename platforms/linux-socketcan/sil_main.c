/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file sil_main.c
 * @brief open-crank-sil: the simulated machine (plant, engine ECU, hoist
 *        pump, operator panel) on a SocketCAN bus, paced by the wall clock.
 *
 *     open-crank-sil vcan0 scenarios/hoist_step.csv [--trace sil.csv]
 *
 * The device under test (open-crank-mc, or your own controller) runs as a
 * separate process or board on the same bus. When the scenario reaches its
 * `end` event the machine keeps running with the last values until Ctrl+C.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <opencrank/runtime/bench.h>
#include <opencrank/runtime/trace.h>

#include "pacer.h"
#include "socketcan.h"

static volatile sig_atomic_t g_running = 1;
static OcScenario g_scenario;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void usage(void) {
    fprintf(stderr, "usage: open-crank-sil <can-interface> <scenario.csv> [--trace <out.csv>]\n"
                    "  example: open-crank-sil vcan0 scenarios/hoist_step.csv\n");
}

int main(int argc, char **argv) {
    const char *ifname = NULL;
    const char *scenario_path = NULL;
    const char *trace_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (argv[i][0] != '-' && ifname == NULL) {
            ifname = argv[i];
        } else if (argv[i][0] != '-' && scenario_path == NULL) {
            scenario_path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (ifname == NULL || scenario_path == NULL) {
        usage();
        return 2;
    }

    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-sil: %s\n", err);
        return 1;
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int sock = oc_can_open(ifname);
    if (sock < 0) {
        perror("open-crank-sil: cannot open CAN interface. Try: sudo modprobe vcan && "
               "sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0");
        return 1;
    }

    FILE *trace = NULL;
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL) {
            perror("open-crank-sil: cannot open trace file");
            close(sock);
            return 1;
        }
        oc_trace_header(trace);
    }

    OcBench bench;
    oc_bench_init(&bench, &OC_BENCH_D936LA6);
    oc_bench_set_scenario(&bench, &g_scenario);
    OcPacer pacer;
    oc_pacer_init(&pacer, bench.cfg->step_ms);

    printf("open-crank-sil: [%s] on %s, scenario %s (Ctrl+C to stop)\n", bench.cfg->plant->name, ifname,
           scenario_path);

    int end_reported = 0;
    while (g_running) {
        J1939Frame f;
        while (oc_can_recv(sock, &f) > 0) {
            oc_bench_rx(&bench, &f);
        }

        J1939Frame tx[8];
        const size_t n = oc_bench_step(&bench, tx, sizeof(tx) / sizeof(tx[0]));
        for (size_t i = 0; i < n; i++) {
            (void)oc_can_send(sock, &tx[i]);
        }
        if (trace != NULL) {
            oc_trace_row(trace, &bench, NULL);
        }

        if (bench.t_ms % 500U == 0U) {
            printf("[%7.2fs] rpm %7.1f | fuel %5.1f %% | load %6.1f Nm | ECU %-8s | speed set by 0x%02X | pump %5.1f %%\n",
                   oc_bench_time_s(&bench), bench.plant.rpm, bench.plant.fuel_pct, bench.load_torque_nm,
                   oc_ecu_state_name(bench.ecu.state), (unsigned)bench.ecu.controlling_address,
                   bench.pump_cmd_applied_pct);
            fflush(stdout);
        }
        if (!end_reported && oc_bench_scenario_done(&bench)) {
            printf("open-crank-sil: scenario ended at %.1f s; holding the last values (Ctrl+C to stop)\n",
                   oc_bench_time_s(&bench));
            end_reported = 1;
        }
        oc_pacer_wait(&pacer);
    }

    printf("open-crank-sil: stopped (%u late steps)\n", (unsigned)pacer.overruns);
    if (trace != NULL) {
        fclose(trace);
    }
    close(sock);
    return 0;
}
