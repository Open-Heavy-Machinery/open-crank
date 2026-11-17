/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file sil_main.c
 * @brief Real-time engine plant and J1939 CAN bus simulation entry point using Linux SocketCAN.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "socketcan.h"

#include <opencrank/plant.h>
#include <opencrank/plants/d936la6.h>
#include <opencrank/j1939.h>
#include <opencrank/engine_ecu.h>
#include <opencrank/loads/hydraulic_pump.h>

/** @brief Global flag controlling the main simulation loop execution (cleared on SIGINT). */
static volatile sig_atomic_t g_running = 1;

/**
 * @brief Signal handler for graceful termination on SIGINT (Ctrl+C).
 * @param sig Signal number received.
 */
static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

/**
 * @brief Transmits an 8-byte SAE J1939 extended CAN frame over the open socket.
 * 
 * @param sock Open SocketCAN file descriptor.
 * @param id 29-bit J1939 CAN identifier (without the Extended Frame Format flag).
 * @param data 8-byte CAN payload buffer.
 */
static void can_send_j1939(int sock, uint32_t id, const uint8_t data[8]) {
    struct can_frame frame = {0};
    frame.can_id = id | CAN_EFF_FLAG; /* Set 29-bit Extended Frame Format flag */
    frame.can_dlc = 8;
    memcpy(frame.data, data, 8);
    if (write(sock, &frame, sizeof(frame)) < 0) {
        /* bus off or queue full: the frame is lost, as on a real bus */
    }
}

/**
 * @brief Main simulation loop: initializes the engine plant, hydraulic load, and SocketCAN interface,
 *        then executes a deterministic 100 Hz real-time simulation step.
 */
int main(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "vcan0";
    signal(SIGINT, handle_sigint);

    int sock = oc_can_open(ifname);
    if (sock < 0) {
        perror("Error opening CAN interface. Run: sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0");
        return 1;
    }

    /* The simulated world: plant physics, the engine's own ECU, and the hoist pump. */
    PlantState engine;
    plant_init(&engine, &PLANT_D936LA6);
    EngineEcu ecu;
    engine_ecu_init(&ecu, &ENGINE_ECU_D936LA6);
    HydraulicPumpState pump;
    hydraulic_pump_init(&pump, &HYDRAULIC_PUMP_HOIST);

    const uint32_t dt_ms = 10U;
    const double dt = (double)dt_ms / 1000.0;
    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    printf("open-crank: [%s] running on %s (Ctrl+C to stop)\n", PLANT_D936LA6.name, ifname);

    uint64_t tick = 0;
    while (g_running) {
        /* 1. Drain the receive queue. Speed requests (TSC1) are not wired to the
         *    engine ECU yet; that arrives with the standard TSC1 codec. */
        struct can_frame rx;
        while (read(sock, &rx, sizeof(rx)) > 0) {
        }

        /* 2. Hard-coded operating profile (moves to a scenario file later):
         *    key on, hold START from 0.5 s to 3 s, hoist from 5 s to 15 s. */
        double sim_time_s = (double)tick * dt;
        EngineEcuInputs ecu_in = {
            .key_on = true,
            .start = (sim_time_s >= 0.5 && sim_time_s < 3.0),
            .rpm = engine.rpm,
        };
        double hoist_demand_pct = (sim_time_s >= 5.0 && sim_time_s < 15.0) ? 85.0 : 0.0;

        /* 3. Engine ECU decides, the load answers, the plant integrates. */
        EngineEcuOutputs ecu_out;
        engine_ecu_step(&ecu, &ENGINE_ECU_D936LA6, &ecu_in, dt_ms, &ecu_out);
        double load_nm = hydraulic_pump_step(&pump, &HYDRAULIC_PUMP_HOIST, hoist_demand_pct, 150.0, engine.rpm, dt);
        PlantInputs plant_in = {
            .fuel_pct = ecu_out.fuel_pct,
            .load_torque_nm = load_nm,
            .starter_engaged = ecu_out.starter_engaged,
        };
        plant_step(&engine, &PLANT_D936LA6, &plant_in, dt);

        /* 4. EEC1 every 20 ms; ET1 and EFL/P1 every 500 ms. */
        if (tick % 2 == 0) {
            uint32_t id;
            uint8_t data[8];
            j1939_encode_eec1(&engine, &id, data);
            can_send_j1939(sock, id, data);
        }
        if (tick % 50 == 0) {
            uint32_t id;
            uint8_t data[8];
            j1939_encode_et1(&engine, &id, data);
            can_send_j1939(sock, id, data);
            j1939_encode_eflp1(&engine, &id, data);
            can_send_j1939(sock, id, data);

            printf("[%5.1fs] rpm %7.1f | fuel %5.1f %% | brake %6.1f Nm | load %6.1f Nm | starter %d | oil %5.1f kPa\n",
                   sim_time_s, engine.rpm, engine.fuel_pct, engine.brake_torque_nm, load_nm,
                   (int)ecu_out.starter_engaged, engine.oil_pressure_kpa);
            fflush(stdout);
        }

        /* 5. Wall-clock pacing: one step every 10 ms. */
        tick++;
        next_tick.tv_nsec += 10000000L;
        if (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_sec += 1;
            next_tick.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    }

    printf("\nShutting down cleanly.\n");
    close(sock);
    return 0;
}
