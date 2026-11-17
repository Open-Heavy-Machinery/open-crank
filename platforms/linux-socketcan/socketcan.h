/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file socketcan.h
 * @brief Thin Linux SocketCAN glue: J1939 frames over a raw CAN socket.
 */

#ifndef OC_PLATFORM_SOCKETCAN_H
#define OC_PLATFORM_SOCKETCAN_H

#include <stdbool.h>

#include <opencrank/j1939.h>

/**
 * @brief Opens a non-blocking raw CAN socket bound to @p ifname.
 * @return The socket file descriptor, or -1 on error (errno is set).
 */
int oc_can_open(const char *ifname);

/** @brief Sends one 29-bit frame. Returns false if the frame was not queued. */
bool oc_can_send(int sock, const J1939Frame *f);

/**
 * @brief Receives one 29-bit frame without blocking. Standard (11-bit),
 *        remote and error frames are skipped.
 * @return 1 if @p f was filled, 0 if the queue is empty, -1 on error.
 */
int oc_can_recv(int sock, J1939Frame *f);

#endif /* OC_PLATFORM_SOCKETCAN_H */
