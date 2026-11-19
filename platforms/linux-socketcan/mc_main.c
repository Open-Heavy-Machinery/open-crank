/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file mc_main.c
 * @brief open-crank-mc: the reference machine controller on a SocketCAN bus.
 *
 * Thin glue: every received frame goes to the portable controller core,
 * every 10 ms the core runs and its frames go out. The operator's target
 * speed and hoist lever arrive on the bus (OC_OPERATOR), like on a real
 * machine with a CAN joystick.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>

#include <opencrank/machine_ctrl.h>

#include <opencrank/can_otk.h>
#include <opentick/host/pacer.h>
#include <opentick/host/socketcan.h>

#define MC_PERIOD_MS 10U

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "vcan0";
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    OtkSocketCan sock;
    if (!otk_socketcan_open(&sock, ifname)) {
        perror("open-crank-mc: cannot open CAN interface");
        return 1;
    }

    McCtx mc;
    mc_init(&mc, &MC_DEFAULT_PARAMS);
    OtkPacer pacer;
    otk_pacer_init(&pacer, otk_ns_from_ms(MC_PERIOD_MS));

    printf("open-crank-mc: machine controller 0x%02X on %s (Ctrl+C to stop)\n",
           (unsigned)MC_DEFAULT_PARAMS.source_address, ifname);

    uint32_t now_ms = 0U;
    McState last_state = mc.state;
    while (g_running) {
        OtkCanFrame in;
        while (otk_socketcan_recv(&sock, &in) > 0) {
            J1939Frame f;
            if (oc_frame_from_otk(&in, &f)) {
                mc_handle_frame(&mc, &f, now_ms);
            }
        }

        J1939Frame out[4];
        const size_t n = mc_step(&mc, now_ms, out, sizeof(out) / sizeof(out[0]));
        for (size_t i = 0; i < n; i++) {
            OtkCanFrame o;
            oc_frame_to_otk(&out[i], &o);
            (void)otk_socketcan_send(&sock, &o);
        }

        if (mc.state != last_state) {
            printf("\n[%7.2fs] %s -> %s%s%s\n", (double)now_ms / 1000.0, mc_state_name(last_state),
                   mc_state_name(mc.state), (mc.state == MC_STATE_FAULT) ? ": " : "",
                   (mc.state == MC_STATE_FAULT) ? mc_fault_name(mc.fault) : "");
            last_state = mc.state;
        }
        if (now_ms % 500U == 0U) {
            printf("\r[%7.2fs] %-11s engine %7.1f rpm | target %6.1f | request %6.1f | pump %5.1f %%   ",
                   (double)now_ms / 1000.0, mc_state_name(mc.state), (double)mc.engine_rpm,
                   (double)mc.target_rpm, (double)mc.requested_rpm, (double)mc.pump_cmd_pct);
            fflush(stdout);
        }

        now_ms += MC_PERIOD_MS;
        otk_pacer_wait(&pacer);
    }

    printf("\nopen-crank-mc: stopped (%u late periods)\n", (unsigned)pacer.overruns);
    otk_socketcan_close(&sock);
    return 0;
}
