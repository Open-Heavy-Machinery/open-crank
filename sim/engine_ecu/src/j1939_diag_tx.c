/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_diag_tx.c
 * @brief DM1 and BAM, sending side. See j1939_diag_tx.h.
 */

#include <opencrank/j1939_diag_tx.h>

/* An 8-byte frame with every byte "not available". */
static void frame_init(J1939Frame *f, uint32_t id) {
    f->id = id;
    f->dlc = 8U;
    for (size_t i = 0U; i < 8U; i++) {
        f->data[i] = 0xFFU;
    }
}

static uint8_t lamp_bits(uint8_t lamp, uint8_t shift) {
    return (uint8_t)((uint8_t)(lamp & 0x03U) << shift);
}

size_t j1939_encode_dm1(const J1939Dm1 *m, uint8_t *out, size_t cap) {
    const size_t n = (m->n_dtc > J1939_DM1_MAX_DTC) ? (size_t)J1939_DM1_MAX_DTC : (size_t)m->n_dtc;
    const size_t used = 2U + (4U * ((n == 0U) ? 1U : n));
    const size_t len = (used < 8U) ? 8U : used;
    size_t written = 0U;
    if (cap >= len) {
        for (size_t i = 0U; i < len; i++) {
            out[i] = 0xFFU;
        }
        out[0] = (uint8_t)(lamp_bits(m->mil, 6U) | lamp_bits(m->red_stop, 4U) | lamp_bits(m->amber_warning, 2U) |
                           lamp_bits(m->protect, 0U));
        if (n == 0U) {
            out[2] = 0U; /* no active DTC: SPN 0, FMI 0, count 0 */
            out[3] = 0U;
            out[4] = 0U;
            out[5] = 0U;
        }
        for (size_t k = 0U; k < n; k++) {
            const J1939Dtc *d = &m->dtc[k];
            uint8_t *p = &out[2U + (4U * k)];
            p[0] = (uint8_t)(d->spn & 0xFFU);
            p[1] = (uint8_t)((d->spn >> 8U) & 0xFFU);
            p[2] = (uint8_t)((uint8_t)(((d->spn >> 16U) & 0x07U) << 5U) | (uint8_t)(d->fmi & 0x1FU));
            p[3] = (uint8_t)(d->oc & 0x7FU); /* conversion method 0 */
        }
        written = len;
    }
    return written;
}

size_t j1939_bam_encode(uint32_t pgn, const uint8_t *data, size_t len, uint8_t src, J1939Frame *out, size_t cap) {
    const size_t packets = (len + 6U) / 7U;
    size_t n = 0U;
    if ((len > 8U) && (len <= J1939_DM1_MAX_LEN) && (cap >= (packets + 1U))) {
        J1939Frame *cm = &out[0];
        frame_init(cm, j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_TP_CM, J1939_ADDR_GLOBAL, src));
        cm->data[0] = J1939_TP_CM_BAM;
        cm->data[1] = (uint8_t)(len & 0xFFU);
        cm->data[2] = (uint8_t)((len >> 8U) & 0xFFU);
        cm->data[3] = (uint8_t)packets;
        cm->data[4] = 0xFFU;
        cm->data[5] = (uint8_t)(pgn & 0xFFU);
        cm->data[6] = (uint8_t)((pgn >> 8U) & 0xFFU);
        cm->data[7] = (uint8_t)((pgn >> 16U) & 0xFFU);
        for (size_t k = 0U; k < packets; k++) {
            J1939Frame *dt = &out[1U + k];
            frame_init(dt, j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_TP_DT, J1939_ADDR_GLOBAL, src));
            dt->data[0] = (uint8_t)(k + 1U);
            for (size_t i = 0U; i < 7U; i++) {
                const size_t at = (7U * k) + i;
                dt->data[1U + i] = (at < len) ? data[at] : 0xFFU;
            }
        }
        n = packets + 1U;
    }
    return n;
}
