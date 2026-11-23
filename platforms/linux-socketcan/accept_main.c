/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file accept_main.c
 * @brief open-crank-accept: the simulated machine on a SocketCAN bus, as
 *        open-crank-sil, graded by the acceptance oracle from the frames
 *        the device under test sends. See docs/acceptance.md.
 *
 *     open-crank-accept vcan0 scenarios/hoist_step.csv [--criteria <file>] [--dut <address>] [--trace <out.csv>]
 *
 * Every received frame goes to the oracle first and then to the bench, so
 * the oracle sees what the device under test sends even when fault
 * injection (fault.mc_tx_lost) keeps it from the engine. The oracle
 * evaluates once per step, after the received frames. The run ends when the
 * scenario reaches `end`.
 *
 * Exit codes: 0 pass, 1 fail, 2 usage, format, I/O or Ctrl+C, 3 invalid
 * (late steps: the timing of the run cannot be trusted).
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opencrank/accept.h>
#include <opencrank/runtime/bench.h>
#include <opencrank/runtime/trace.h>

#include <opencrank/can_otk.h>
#include <opentick/host/pacer.h>
#include <opentick/host/socketcan.h>
#include <opentick/oracle.h>

#define EXIT_PASS 0
#define EXIT_FAIL 1
#define EXIT_ERROR 2
#define EXIT_INVALID 3

static volatile sig_atomic_t g_running = 1;
static OcScenario g_scenario;
static OtkOracle g_oracle;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: open-crank-accept <can-interface> <scenario.csv> [--criteria <file>] [--dut <address>]"
            " [--trace <out.csv>]\n"
            "  --criteria  defaults to the scenario's path with .csv replaced by .accept.csv\n"
            "  --dut       source address of the device under test, default 0x%02X\n"
            "  example: open-crank-accept vcan0 scenarios/hoist_step.csv\n",
            (unsigned)OC_ACCEPT_DUT_DEFAULT);
}

static int parse_address(const char *s, uint8_t *out) {
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || v > 0xFDUL) {
        return 0;
    }
    *out = (uint8_t)v;
    return 1;
}

int main(int argc, char **argv) {
    const char *ifname = NULL;
    const char *scenario_path = NULL;
    const char *criteria_path = NULL;
    const char *trace_path = NULL;
    uint8_t dut = OC_ACCEPT_DUT_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            criteria_path = argv[++i];
        } else if (strcmp(argv[i], "--dut") == 0 && i + 1 < argc) {
            if (!parse_address(argv[++i], &dut)) {
                fprintf(stderr, "open-crank-accept: bad address '%s' (0x00..0xFD)\n", argv[i]);
                return EXIT_ERROR;
            }
        } else if (argv[i][0] != '-' && ifname == NULL) {
            ifname = argv[i];
        } else if (argv[i][0] != '-' && scenario_path == NULL) {
            scenario_path = argv[i];
        } else {
            usage();
            return EXIT_ERROR;
        }
    }
    if (ifname == NULL || scenario_path == NULL) {
        usage();
        return EXIT_ERROR;
    }

    char default_criteria[4096];
    if (criteria_path == NULL) {
        const size_t n = strlen(scenario_path);
        if (n < 4U || strcmp(&scenario_path[n - 4U], ".csv") != 0 || n + 8U > sizeof(default_criteria)) {
            fprintf(stderr, "open-crank-accept: the scenario's name does not end in .csv; give --criteria\n");
            return EXIT_ERROR;
        }
        (void)snprintf(default_criteria, sizeof(default_criteria), "%.*s.accept.csv", (int)(n - 4U), scenario_path);
        criteria_path = default_criteria;
    }

    char err[600];
    if (!oc_scenario_load(scenario_path, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "open-crank-accept: %s: %s\n", scenario_path, err);
        return EXIT_ERROR;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "open-crank-accept: %s has no end event; the run would never finish\n", scenario_path);
        return EXIT_ERROR;
    }

    OcBench bench;
    oc_bench_init(&bench, &OC_BENCH_D936LA6);
    oc_bench_set_scenario(&bench, &g_scenario);

    OcAccept accept;
    OtkOracleTable table;
    oc_accept_init(&accept, dut, &table);
    if (!otk_oracle_load(&g_oracle, criteria_path, &table, &g_scenario, &OC_SIGNAL_TABLE,
                         otk_ns_from_ms(bench.cfg->step_ms), err, sizeof(err))) {
        fprintf(stderr, "open-crank-accept: %s: %s\n", criteria_path, err);
        return EXIT_ERROR;
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    OtkSocketCan sock;
    if (!otk_socketcan_open(&sock, ifname)) {
        perror("open-crank-accept: cannot open CAN interface. Try: sudo modprobe vcan && "
               "sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0");
        return EXIT_ERROR;
    }

    FILE *trace = NULL;
    OtkTrace tr;
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL) {
            perror("open-crank-accept: cannot open trace file");
            otk_socketcan_close(&sock);
            return EXIT_ERROR;
        }
        if (!oc_trace_open(&tr, trace)) {
            fprintf(stderr, "open-crank-accept: trace: %s\n", otk_trace_error(&tr));
            fclose(trace);
            otk_socketcan_close(&sock);
            return EXIT_ERROR;
        }
    }

    printf("open-crank-accept: [%s] on %s, scenario %s, criteria %s, device under test 0x%02X\n",
           bench.cfg->plant->name, ifname, scenario_path, criteria_path, (unsigned)dut);
    fflush(stdout);

    OtkPacer pacer;
    otk_pacer_init(&pacer, otk_ns_from_ms(bench.cfg->step_ms));
    uint32_t dut_frames = 0U;
    while (g_running) {
        OtkCanFrame in;
        while (otk_socketcan_recv(&sock, &in) > 0) {
            otk_oracle_frame(&g_oracle, &in, otk_ns_from_ms(bench.t_ms));
            J1939Frame f;
            if (oc_frame_from_otk(&in, &f)) {
                if (j1939_id_source(f.id) == dut) {
                    dut_frames++;
                }
                oc_bench_rx(&bench, &f);
            }
        }
        otk_oracle_step(&g_oracle, otk_ns_from_ms(bench.t_ms));
        if (oc_bench_scenario_done(&bench)) {
            break;
        }

        J1939Frame tx[8];
        const size_t n = oc_bench_step(&bench, tx, sizeof(tx) / sizeof(tx[0]));
        for (size_t i = 0; i < n; i++) {
            OtkCanFrame out;
            oc_frame_to_otk(&tx[i], &out);
            (void)otk_socketcan_send(&sock, &out);
        }
        if (trace != NULL) {
            oc_trace_row(&tr, &bench, NULL);
        }
        otk_pacer_wait(&pacer);
    }

    if (trace != NULL) {
        fclose(trace);
    }
    otk_socketcan_close(&sock);

    if (!g_running) {
        fprintf(stderr, "open-crank-accept: interrupted at %.2f s; no verdict\n", oc_bench_time_s(&bench));
        return EXIT_ERROR;
    }

    const bool pass = otk_oracle_report(&g_oracle, stdout);
    const size_t total = otk_oracle_count(&g_oracle);
    const size_t failed = otk_oracle_failures(&g_oracle);
    const unsigned late = (unsigned)pacer.overruns;

    if (dut_frames == 0U) {
        printf("open-crank-accept: no frames from 0x%02X were seen; is the device under test on the bus?\n",
               (unsigned)dut);
    }
    if (otk_oracle_failed(&g_oracle, "S2") || otk_oracle_failed(&g_oracle, "S1")) {
        printf("If the device under test is a board, reset it before each run.\n");
    }
    if (late > 0U) {
        printf("open-crank-accept: INVALID: %u late steps; the timing of this run cannot be trusted\n", late);
        return EXIT_INVALID;
    }
    if (pass) {
        printf("open-crank-accept: PASS (%zu of %zu criteria), 0 late steps\n", total, total);
        return EXIT_PASS;
    }
    printf("open-crank-accept: FAIL (%zu of %zu criteria failed), %u late steps\n", failed, total, late);
    return EXIT_FAIL;
}
