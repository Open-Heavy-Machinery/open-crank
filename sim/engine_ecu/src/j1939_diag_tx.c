/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_diag_tx.c
 * @brief BAM, sending side. See j1939_diag_tx.h.
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
