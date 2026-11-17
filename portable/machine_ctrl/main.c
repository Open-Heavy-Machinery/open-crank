/**
 * @file main.c
 * @brief Reference Electronic Control Unit (ECU) firmware implementing a closed-loop PI speed governor
 *        over SAE J1939 (SocketCAN).
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

/** @name Standard 29-bit J1939 Extended CAN Identifiers
 * @{
 */
#define J1939_ID_TSC1 0x0C000003U /**< Torque/Speed Control 1 (transmitted to engine plant) */
#define J1939_ID_EEC1 0x0CF00400U /**< Electronic Engine Controller 1 (received from engine plant) */
/** @} */

/** @brief Global flag controlling the main ECU control loop execution (cleared on SIGINT). */
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

    /* Configure socket for non-blocking I/O so polling does not stall the control loop */
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
 * @brief Main ECU control loop: receives engine speed telemetry via EEC1, executes a 50 Hz
 *        PI speed governor with integral anti-windup, and broadcasts TSC1 throttle commands.
 */
int main(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "vcan0";
    double target_rpm = (argc > 2) ? atof(argv[2]) : 1500.0;

    signal(SIGINT, handle_sigint);

    /* Initialize SocketCAN interface */
    int sock = can_open(ifname);
    if (sock < 0) {
        perror("Error opening CAN interface");
        return 1;
    }

    /* PI speed controller tuning parameters and discrete time step */
    const double kp = 0.08;  /**< Proportional gain (%/RPM) */
    const double ki = 0.15;  /**< Integral gain (%/(RPM·s)) */
    const double dt = 0.020; /**< Control loop period: 20 ms (50 Hz, standard J1939 TSC1 rate) */

    double measured_rpm = 0.0;
    double integral_error = 0.0;

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    printf("open-crank Reference ECU: Target = %.1f RPM on %s\n", target_rpm, ifname);

    uint64_t tick = 0;
    while (g_running) {
        /* 1. Drain incoming CAN frames and extract engine speed (SPN 190) from EEC1 */
        struct can_frame rx;
        while (read(sock, &rx, sizeof(rx)) > 0) {
            if ((rx.can_id & CAN_EFF_FLAG) &&
                ((rx.can_id & CAN_EFF_MASK) == J1939_ID_EEC1) &&
                rx.can_dlc >= 5) {
                /* SPN 190: Engine Speed (bytes 3-4 little-endian, resolution: 0.125 RPM/bit) */
                uint16_t raw_rpm = (uint16_t)rx.data[3] | ((uint16_t)rx.data[4] << 8);
                measured_rpm = (double)raw_rpm * 0.125;
            }
        }

        /* 2. Execute discrete PI speed control loop with integral anti-windup */
        double error = target_rpm - measured_rpm;
        integral_error += error * dt;

        /* Clamp integral accumulator to prevent windup and excessive overshoot */
        if (integral_error > 500.0) {
            integral_error = 500.0;
        }
        if (integral_error < -500.0) {
            integral_error = -500.0;
        }

        /* Compute and clamp percentage throttle demand [0.0, 100.0] */
        double throttle_cmd = (kp * error) + (ki * integral_error);
        if (throttle_cmd > 100.0) {
            throttle_cmd = 100.0;
        }
        if (throttle_cmd < 0.0) {
            throttle_cmd = 0.0;
        }

        /* 3. Encode and transmit J1939 TSC1 control frame every 20 ms (50 Hz) */
        struct can_frame tx = {0};
        tx.can_id = J1939_ID_TSC1 | CAN_EFF_FLAG;
        tx.can_dlc = 8;
        memset(tx.data, 0xFF, 8); /* Fill unused payload bytes with 0xFF ("Not Available") */

        tx.data[0] = 0x01; /* Byte 0: Override control mode (01b = Speed/Torque control active) */
        tx.data[1] = (uint8_t)((throttle_cmd / 100.0) * 250.0); /* Byte 1: Scaled command [0..250] */

        (void)write(sock, &tx, sizeof(tx));

        /* Print telemetry and controller status every 500 ms (25 ticks * 20 ms) */
        if (tick % 25 == 0) {
            printf("\rTarget: %6.1f RPM | Measured: %6.1f RPM | Error: %+6.1f | Cmd: %5.1f%% ",
                   target_rpm, measured_rpm, error, throttle_cmd);
            fflush(stdout);
        }

        /* 4. Sleep until the next exact 20 ms monotonic deadline */
        tick++;
        next_tick.tv_nsec += 20000000L;
        if (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_sec += 1;
            next_tick.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    }

    printf("\nECU stopped.\n");
    close(sock);
    return 0;
}
