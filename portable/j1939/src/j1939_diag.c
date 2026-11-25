/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_diag.c
 * @brief DM1 decoding and the BAM receiver. See j1939_diag.h.
 */

#include <opencrank/j1939_diag.h>

#include "j1939_scaling.h"

#define SPN_NA 0x7FFFFU /* 19 bits all set: SPN not available */
#define FMI_NA 0x1FU

/* ---------------------------------------------------------------- DM1 */

bool j1939_decode_dm1(const uint8_t *data, size_t len, J1939Dm1 *m) {
    const bool ok = len >= 6U; /* lamps and one DTC at least */
    if (ok) {
        m->mil = (uint8_t)((data[0] >> 6U) & 0x03U);
        m->red_stop = (uint8_t)((data[0] >> 4U) & 0x03U);
        m->amber_warning = (uint8_t)((data[0] >> 2U) & 0x03U);
        m->protect = (uint8_t)(data[0] & 0x03U);
        m->n_dtc = 0U;
        for (size_t at = 2U; (at + 4U) <= len; at += 4U) {
            const uint8_t *p = &data[at];
            const uint32_t spn = (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)((p[2] >> 5U) & 0x07U) << 16U);
            const uint8_t fmi = (uint8_t)(p[2] & 0x1FU);
            const bool none = (spn == 0U) && (fmi == 0U);
            const bool na = (spn == SPN_NA) && (fmi == FMI_NA); /* padding of the last packet */
            if (!none && !na && (m->n_dtc < J1939_DM1_MAX_DTC)) {
                m->dtc[m->n_dtc].spn = spn;
                m->dtc[m->n_dtc].fmi = fmi;
                m->dtc[m->n_dtc].oc = (uint8_t)(p[3] & 0x7FU);
                m->n_dtc++;
            }
        }
    }
    return ok;
}

/* ---------------------------------------------------------------- BAM */

void j1939_bam_rx_init(J1939BamRx *rx) {
    rx->active = false;
    rx->pgn = 0U;
    rx->size = 0U;
    rx->packets = 0U;
    rx->next_seq = 0U;
    rx->last_ms = 0U;
    for (size_t i = 0U; i < sizeof(rx->buf); i++) {
        rx->buf[i] = 0xFFU;
    }
}

static void bam_start(J1939BamRx *rx, const J1939Frame *f, uint32_t now_ms) {
    const uint16_t size = j1939_get_u16(&f->data[1]);
    const uint8_t packets = f->data[3];
    const bool fits = (size > 8U) && (size <= J1939_DM1_MAX_LEN);
    const bool consistent = (uint32_t)packets == (((uint32_t)size + 6U) / 7U);
    rx->active = fits && consistent; /* a BAM this receiver cannot hold is ignored */
    rx->pgn = (uint32_t)f->data[5] | ((uint32_t)f->data[6] << 8U) | ((uint32_t)f->data[7] << 16U);
    rx->size = size;
    rx->packets = packets;
    rx->next_seq = 1U;
    rx->last_ms = now_ms;
}

static bool bam_data(J1939BamRx *rx, const J1939Frame *f, uint32_t now_ms) {
    bool done = false;
    const bool late = (uint32_t)(now_ms - rx->last_ms) > J1939_TP_T1_MS; /* wrap-safe */
    const uint8_t seq = f->data[0];
    if (late || (seq != rx->next_seq)) {
        rx->active = false; /* J1939-21: drop the message; wait for the next TP.CM */
    } else {
        const size_t base = 7U * ((size_t)seq - 1U);
        for (size_t i = 0U; i < 7U; i++) {
            rx->buf[base + i] = f->data[1U + i];
        }
        rx->last_ms = now_ms;
        rx->next_seq++;
        if (seq == rx->packets) {
            rx->active = false;
            done = true;
        }
    }
    return done;
}

bool j1939_bam_rx_frame(J1939BamRx *rx, const J1939Frame *f, uint32_t now_ms, uint32_t *pgn) {
    bool done = false;
    if (f->dlc == 8U) {
        const uint32_t frame_pgn = j1939_id_pgn(f->id);
        const bool to_all = j1939_id_dest(f->id) == J1939_ADDR_GLOBAL;
        if ((frame_pgn == J1939_PGN_TP_CM) && to_all && (f->data[0] == J1939_TP_CM_BAM)) {
            bam_start(rx, f, now_ms);
        } else if ((frame_pgn == J1939_PGN_TP_DT) && to_all && rx->active) {
            done = bam_data(rx, f, now_ms);
            if (done) {
                *pgn = rx->pgn;
            }
        } else {
            /* not part of a BAM */
        }
    }
    return done;
}
