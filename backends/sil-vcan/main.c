/**
 * @file main.c
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

#include <opencrank/plant.h>
#include <opencrank/plants/d936la6.h>
#include <opencrank/j1939.h>
#include <opencrank/loads/hydraulic_pto.h>

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
 * @brief Opens and binds a non-blocking raw SocketCAN socket to the specified network interface.
 * 
 * @param ifname Name of the CAN interface (e.g., "vcan0", "can0").
 * @return Valid socket file descriptor on success, or -1 on error.
 */
static int can_open(const char *ifname) {
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        return -1;
    }

    /* Configure socket for non-blocking I/O so polling does not stall the simulation step */
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    /* Resolve network interface index by name */
    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        close(s);
        return -1;
    }

    /* Bind socket to the resolved CAN interface */
    struct sockaddr_can addr = {0};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
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
    (void)write(sock, &frame, sizeof(frame));
}

/**
 * @brief Main simulation loop: initializes the engine plant, hydraulic load, and SocketCAN interface,
 *        then executes a deterministic 100 Hz real-time simulation step.
 */
int main(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "vcan0";
    signal(SIGINT, handle_sigint);

    /* Initialize SocketCAN interface */
    int sock = can_open(ifname);
    if (sock < 0) {
        perror("Error opening CAN interface. Run: sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0");
        return 1;
    }

    /* Initialize engine plant state at nominal idle speed */
    PlantState engine;
    PlantInputs inputs = { .throttle_pct = 0.0, .load_torque_nm = 0.0, .starter_active = false };
    plant_init(&engine, &PLANT_D936LA6, PLANT_D936LA6.idle_rpm);

    /* Configure deterministic 10 ms (100 Hz) fixed time step */
    const double dt = 0.010;
    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    printf("open-crank: [%s] running on %s (Ctrl+C to stop)\n", PLANT_D936LA6.name, ifname);

    uint64_t tick = 0;

    /* Initialize external hydraulic PTO load */
    HydraulicPto crane_pto;
    hydraulic_pto_init(&crane_pto);

    while (g_running) {
        /* 1. Drain all pending incoming CAN frames and decode J1939 commands */
        struct can_frame rx;
        while (read(sock, &rx, sizeof(rx)) > 0) {
            if (rx.can_id & CAN_EFF_FLAG) {
                j1939_decode_rx(rx.can_id & CAN_EFF_MASK, rx.data, rx.can_dlc, &inputs);
            }
        }

        /* 2. Advance deterministic physical simulation step */
        double sim_time_s = (double)tick * dt;
        double crane_lift_demand = 0.0;

        /* Step A: Apply scripted crane hoist duty cycle between t = 5.0 s and t = 15.0 s */
        if (sim_time_s >= 5.0 && sim_time_s < 15.0) {
            crane_lift_demand = 0.85;
        }

        /* Step B: Compute resistive hydraulic pump torque for the current demand and engine speed */
        inputs.load_torque_nm = hydraulic_pto_step(&crane_pto, crane_lift_demand, engine.rpm, dt);

        /* Step C: Integrate engine rotational and thermal dynamics under the coupled load */
        plant_step(&engine, &PLANT_D936LA6, &inputs, dt);

        /* 3. Broadcast EEC1 (Engine Speed) every 20 ms (50 Hz) */
        if (tick % 2 == 0) {
            uint32_t id;
            uint8_t data[8];
            j1939_encode_eec1(&engine, &id, data);
            can_send_j1939(sock, id, data);
        }

        /* 4. Broadcast ET1 (Coolant Temp) and EFLP1 (Oil Pressure) every 500 ms (2 Hz) */
        if (tick % 50 == 0) {
            uint32_t id;
            uint8_t data[8];
            j1939_encode_et1(&engine, &id, data);
            can_send_j1939(sock, id, data);

            j1939_encode_eflp1(&engine, &id, data);
            can_send_j1939(sock, id, data);

            printf("\r[%4.1fs] RPM: %7.1f | Thr: %5.1f%% | Tq: %6.1f Nm | Load: %5.1f Nm | Oil: %5.1f kPa\n",
                   sim_time_s, engine.rpm, inputs.throttle_pct, engine.indicated_tq_nm,
                   inputs.load_torque_nm, engine.oil_pressure_kpa);
            fflush(stdout);
        }

        /* 5. Sleep until the next exact 10 ms monotonic deadline */
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
