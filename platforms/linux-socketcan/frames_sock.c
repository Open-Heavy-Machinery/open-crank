/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file frames_sock.c
 * @brief Frame log for the SocketCAN runners. See frames_sock.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "frames_sock.h"

#include <errno.h>
#include <string.h>
#include <time.h>

bool oc_frames_sock_open(OcFramesSock *fs, const char *path, OtkSocketCan *sock, OtkNs step, const char *source) {
    memset(fs, 0, sizeof(*fs));
    fs->sock = sock;
    if (!otk_socketcan_enable_timestamps(sock, true)) {
        fprintf(stderr, "%s: --frames: cannot turn on kernel timestamps (%s)\n", source, strerror(errno));
        return false;
    }
    const OtkFrameLogConfig cfg = { step, otk_socketcan_bus_clock(sock, true), false, 0U, source };
    fs->fp = fopen(path, "w");
    if (fs->fp == NULL || !otk_framelog_init(&fs->log, fs->fp, &cfg)) {
        fprintf(stderr, "%s: cannot write %s\n", source, path);
        if (fs->fp != NULL) {
            fclose(fs->fp);
            fs->fp = NULL;
        }
        return false;
    }
    return true;
}

void oc_frames_sock_anchor(OcFramesSock *fs) {
    fs->anchor_ns = otk_socketcan_clock_ns();
}

OtkNs oc_frames_sock_rx_time(const OcFramesSock *fs, const OtkSocketCanRxInfo *rx) {
    return rx->has_time ? otk_socketcan_run_time(rx->time_ns, fs->anchor_ns) : OTK_FRAMELOG_NO_TIME;
}

static void write_row(OcFramesSock *fs, const OcSentFrame *s, OtkNs t_bus) {
    const OtkFrameRow row = { s->info.t, t_bus, OTK_FRAME_TO_DUT, s->info.link, &s->frame, s->info.fault };
    (void)otk_framelog_frame(&fs->log, &row); /* an error is sticky: reported at the end */
}

void oc_frames_sock_sent(OcFramesSock *fs, const OtkCanFrame *f, const OcBenchTxInfo *info, bool sent) {
    const OcSentFrame s = { *f, *info };
    if (!sent || fs->n_pending >= OC_FRAMES_SOCK_PENDING) {
        write_row(fs, &s, OTK_FRAMELOG_NO_TIME); /* no copy will come back, or no room to wait for it */
        return;
    }
    fs->pending[fs->n_pending++] = s;
}

static bool same(const OtkCanFrame *a, const OtkCanFrame *b) {
    return a->id == b->id && a->flags == b->flags && a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

void oc_frames_sock_own(OcFramesSock *fs, const OtkCanFrame *f, const OtkSocketCanRxInfo *rx) {
    /* Own copies come back in send order: the first pending frame equal to it is the one. */
    for (size_t i = 0U; i < fs->n_pending; i++) {
        if (same(&fs->pending[i].frame, f)) {
            write_row(fs, &fs->pending[i], oc_frames_sock_rx_time(fs, rx));
            memmove(&fs->pending[i], &fs->pending[i + 1U], (fs->n_pending - i - 1U) * sizeof(fs->pending[0]));
            fs->n_pending--;
            return;
        }
    }
    fs->unmatched++;
}

bool oc_frames_sock_close(OcFramesSock *fs, const char *who) {
    /* The own copies of the last frames sent: at once on vcan, within a few ms on an adapter. */
    for (int tries = 0; tries < 50 && fs->n_pending > 0U; tries++) {
        OtkCanFrame in;
        OtkSocketCanRxInfo rx;
        while (otk_socketcan_recv_ex(fs->sock, &in, &rx) > 0) {
            if (rx.own) {
                oc_frames_sock_own(fs, &in, &rx);
            }
        }
        const struct timespec ms = { 0, 1000000L };
        (void)nanosleep(&ms, NULL);
    }
    for (size_t i = 0U; i < fs->n_pending; i++) {
        write_row(fs, &fs->pending[i], OTK_FRAMELOG_NO_TIME);
    }
    fs->n_pending = 0U;
    const bool ok = otk_framelog_ok(&fs->log);
    if (!ok) {
        fprintf(stderr, "%s: frame log: %s\n", who, otk_framelog_error(&fs->log));
    }
    if (fs->unmatched != 0U) {
        fprintf(stderr, "%s: frame log: %u own frames matched no sent frame\n", who, (unsigned)fs->unmatched);
    }
    const bool closed = fs->fp == NULL || fclose(fs->fp) == 0;
    fs->fp = NULL;
    return ok && closed && fs->unmatched == 0U;
}
