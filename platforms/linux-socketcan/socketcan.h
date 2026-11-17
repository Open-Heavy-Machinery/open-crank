/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file socketcan.h
 * @brief Thin Linux SocketCAN helpers shared by the SIL executables.
 */

#ifndef OC_PLATFORM_SOCKETCAN_H
#define OC_PLATFORM_SOCKETCAN_H

/**
 * @brief Opens a non-blocking raw CAN socket bound to @p ifname.
 * @return The socket file descriptor, or -1 on error (errno is set).
 */
int oc_can_open(const char *ifname);

#endif /* OC_PLATFORM_SOCKETCAN_H */
