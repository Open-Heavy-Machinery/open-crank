/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file socketcan.c
 * @brief Thin Linux SocketCAN glue. See socketcan.h.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "socketcan.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>

int oc_can_open(const char *ifname) {
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        return -1;
    }

    /* Non-blocking so that draining the receive queue never stalls a step. */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(s);
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        close(s);
        return -1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

bool oc_can_send(int sock, const J1939Frame *f) {
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = (f->id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    frame.can_dlc = (f->dlc <= 8U) ? f->dlc : 8U;
    memcpy(frame.data, f->data, frame.can_dlc);
    /* A full queue or bus-off loses the frame, as on a real bus. */
    return write(sock, &frame, sizeof(frame)) == (ssize_t)sizeof(frame);
}

int oc_can_recv(int sock, J1939Frame *f) {
    for (;;) {
        struct can_frame frame;
        ssize_t n = read(sock, &frame, sizeof(frame));
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        if (n != (ssize_t)sizeof(frame)) {
            continue;
        }
        if (!(frame.can_id & CAN_EFF_FLAG) || (frame.can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG))) {
            continue;
        }
        f->id = frame.can_id & CAN_EFF_MASK;
        f->dlc = (frame.can_dlc <= 8U) ? frame.can_dlc : 8U;
        memset(f->data, 0xFF, sizeof(f->data));
        memcpy(f->data, frame.data, f->dlc);
        return 1;
    }
}
