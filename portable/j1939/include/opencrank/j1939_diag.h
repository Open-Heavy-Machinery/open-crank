/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file j1939_diag.h
 * @brief DM1 (active diagnostic trouble codes, J1939-73) and the BAM
 *        transport protocol that carries it when it is longer than one frame
 *        (J1939-21). Portable zone: depends only on <stdint.h>, <stdbool.h>,
 *        <stddef.h>.
 *
 * DM1, PGN 65226, broadcast, as a byte string (one frame, or reassembled
 * from a BAM):
 *   byte 0   lamps, 2 bits each: MIL (bits 7-6), red stop (5-4), amber
 *            warning (3-2), protect (1-0). 00 off, 01 on.
 *   byte 1   lamp flash status (not used here; sent as 0xFF)
 *   then 4 bytes per DTC:
 *     byte 0 SPN bits 0-7
 *     byte 1 SPN bits 8-15
 *     byte 2 SPN bits 16-18 in bits 7-5, FMI in bits 4-0
 *     byte 3 SPN conversion method in bit 7 (0: this layout), occurrence count in bits 6-0
 * With no active DTC the message is one frame and its DTC is all zero.
 * With more than one DTC it is longer than 8 bytes and travels as a BAM.
 *
 * BAM: a TP.CM frame (PGN 60416, to global, control byte 32) announces the
 * size, the number of packets and the PGN carried; TP.DT frames (PGN 60160)
 * follow, each with a sequence number 1..N and 7 data bytes, the last one
 * padded with 0xFF. J1939-21 sends them 50 to 200 ms apart, and a receiver
 * drops a BAM when no packet comes for T1 (750 ms) or a packet is out of
 * sequence.
 */

#ifndef OPEN_CRANK_J1939_DIAG_H
#define OPEN_CRANK_J1939_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/j1939.h>

#define J1939_PGN_DM1 0x0FECAU   /**< DM1, active diagnostic trouble codes (PGN 65226) */
#define J1939_PGN_TP_CM 0x0EC00U /**< transport protocol, connection management (PGN 60416) */
#define J1939_PGN_TP_DT 0x0EB00U /**< transport protocol, data transfer (PGN 60160) */

#define J1939_TP_CM_BAM 32U     /**< TP.CM control byte: broadcast announce message */
#define J1939_TP_T1_MS 750U     /**< J1939-21 T1: longest gap between two packets of a BAM */

#define J1939_DM1_MAX_DTC 8U                                  /**< most DTCs kept from one DM1 */
#define J1939_DM1_MAX_LEN (2U + (4U * J1939_DM1_MAX_DTC))     /**< 34 bytes */
#define J1939_TP_MAX_PACKETS ((J1939_DM1_MAX_LEN + 6U) / 7U) /**< 5 packets */

/** @brief Lamp status, 2 bits (J1939-73). */
enum {
    J1939_LAMP_OFF = 0,
    J1939_LAMP_ON = 1
};

/** @brief One diagnostic trouble code. */
typedef struct {
    uint32_t spn; /**< suspect parameter number, 19 bits */
    uint8_t fmi;  /**< failure mode identifier, 0..31 */
    uint8_t oc;   /**< occurrence count, 0..126 */
} J1939Dtc;

/** @brief DM1 contents. */
typedef struct {
    uint8_t mil;           /**< malfunction indicator lamp, J1939_LAMP_* */
    uint8_t red_stop;      /**< red stop lamp: stop the machine */
    uint8_t amber_warning; /**< amber warning lamp: run on, but take care */
    uint8_t protect;       /**< engine protect lamp */
    uint8_t n_dtc;         /**< active DTCs kept, 0..J1939_DM1_MAX_DTC */
    J1939Dtc dtc[J1939_DM1_MAX_DTC];
} J1939Dm1;

/**
 * @brief Reads a DM1 from @p len bytes (one frame, or a reassembled BAM).
 *        An all-zero DTC, or one with SPN and FMI "not available", is no DTC.
 *        DTCs past J1939_DM1_MAX_DTC are dropped.
 * @return false if @p len is below 6 (lamps and one DTC)
 */
bool j1939_decode_dm1(const uint8_t *data, size_t len, J1939Dm1 *m);

/** @brief Receiver of the BAMs of one sender. */
typedef struct {
    bool active;       /**< a BAM is being received */
    uint32_t pgn;      /**< PGN it carries */
    uint16_t size;     /**< bytes announced */
    uint8_t packets;   /**< packets announced */
    uint8_t next_seq;  /**< sequence number expected next */
    uint32_t last_ms;  /**< time of the last frame of this BAM */
    uint8_t buf[J1939_TP_MAX_PACKETS * 7U];
} J1939BamRx;

/** @brief Ready to receive; nothing in progress. */
void j1939_bam_rx_init(J1939BamRx *rx);

/**
 * @brief Feeds one frame from the sender this receiver listens to. A TP.CM
 *        BAM starts a message (and drops one in progress). A TP.DT continues
 *        it; one out of sequence, or later than J1939_TP_T1_MS after the
 *        previous frame, drops it. Other frames are ignored.
 * @return true when the last packet arrived: the message is in rx->buf,
 *         rx->size bytes long, and *pgn says what it is
 */
bool j1939_bam_rx_frame(J1939BamRx *rx, const J1939Frame *f, uint32_t now_ms, uint32_t *pgn);

#endif /* OPEN_CRANK_J1939_DIAG_H */
