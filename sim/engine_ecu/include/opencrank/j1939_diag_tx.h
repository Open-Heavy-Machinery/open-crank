/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_diag_tx.h
 * @brief The sending side of BAM, for the simulated engine (and the tests):
 *        the format is in opencrank/j1939_diag.h, whose receiving side is
 *        portable. The DM1 encoder is portable too (the controller sends its
 *        own DM1); the BAM sender is not, because the controller's DM1 always
 *        fits in one frame.
 */

#ifndef OPEN_CRANK_J1939_DIAG_TX_H
#define OPEN_CRANK_J1939_DIAG_TX_H

#include <stddef.h>
#include <stdint.h>

#include <opencrank/j1939_diag.h>

#define J1939_TP_BAM_GAP_MS 50U /**< gap the sender leaves between BAM packets (J1939-21: 50 to 200 ms) */

/**
 * @brief Splits @p len bytes of @p pgn into a BAM from @p src: out[0] is the
 *        TP.CM, out[1..] the TP.DT frames, to be sent J1939_TP_BAM_GAP_MS
 *        apart.
 * @return the number of frames, or 0 if @p len is not 9..J1939_DM1_MAX_LEN
 *         or @p cap is too small
 */
size_t j1939_bam_encode(uint32_t pgn, const uint8_t *data, size_t len, uint8_t src, J1939Frame *out, size_t cap);

#endif /* OPEN_CRANK_J1939_DIAG_TX_H */
