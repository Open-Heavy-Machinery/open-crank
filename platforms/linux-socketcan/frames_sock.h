/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file frames_sock.h
 * @brief Frame log (--frames) for the SocketCAN runners, with kernel times.
 *
 * The bus times come from the kernel: a received frame's timestamp, and for
 * a frame the runner sends, the timestamp of the socket's own copy, which the
 * kernel gives back when the frame leaves on the interface (open-tick
 * socketcan.h, otk_socketcan_enable_timestamps()). Both are taken relative to
 * an anchor read at t = 0, so they are on the run's timebase; on a USB
 * adapter they are when the frame reached or left the host, adapter latency
 * included. A sent frame's row is written when its own copy comes back; a
 * frame that never comes back (the run ended first, or the send failed) is
 * written at the end with no bus time.
 */

#ifndef OPEN_CRANK_FRAMES_SOCK_H
#define OPEN_CRANK_FRAMES_SOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <opencrank/runtime/bench.h>
#include <opentick/framelog.h>
#include <opentick/host/socketcan.h>

#define OC_FRAMES_SOCK_PENDING 256U

typedef struct {
    OtkCanFrame frame;
    OcBenchTxInfo info;
} OcSentFrame;

typedef struct {
    FILE *fp;
    OtkFrameLog log;
    OtkSocketCan *sock;
    uint64_t anchor_ns;
    OcSentFrame pending[OC_FRAMES_SOCK_PENDING]; /**< sent, waiting for their own copy */
    size_t n_pending;
    uint32_t unmatched; /**< own copies with no pending frame: should stay 0 */
} OcFramesSock;

/**
 * @brief Opens @p path, turns on the socket's timestamps and own copies, and
 *        writes the header (bus_clock from otk_socketcan_bus_clock(): kernel
 *        for a paced run).
 * @return false, with a message on stderr, if any of it fails
 */
bool oc_frames_sock_open(OcFramesSock *fs, const char *path, OtkSocketCan *sock, OtkNs step, const char *source);

/** @brief Reads the anchor: call it right before the first step, at t = 0. */
void oc_frames_sock_anchor(OcFramesSock *fs);

/** @brief A received frame's bus time on the run's timebase (or no time). */
OtkNs oc_frames_sock_rx_time(const OcFramesSock *fs, const OtkSocketCanRxInfo *rx);

/** @brief The runner sent @p f (or tried: @p sent false): remember it until its own copy comes back. */
void oc_frames_sock_sent(OcFramesSock *fs, const OtkCanFrame *f, const OcBenchTxInfo *info, bool sent);

/** @brief The socket's own copy of a frame came back: write the row of the frame it belongs to. */
void oc_frames_sock_own(OcFramesSock *fs, const OtkCanFrame *f, const OtkSocketCanRxInfo *rx);

/**
 * @brief Waits up to 50 ms for the own copies of the last frames sent, writes
 *        the frames still waiting (no bus time) and closes the file. Call it
 *        before closing the socket.
 * @return false, with a message on stderr, if the log has an error
 */
bool oc_frames_sock_close(OcFramesSock *fs, const char *who);

#endif /* OPEN_CRANK_FRAMES_SOCK_H */
