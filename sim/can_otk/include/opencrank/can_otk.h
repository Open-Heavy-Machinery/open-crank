/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file can_otk.h
 * @brief J1939 frames to and from open-tick's CAN frames.
 *
 * portable/ (which goes to firmware) keeps its own J1939Frame and never sees
 * open-tick; the conversion happens here, on the host side.
 */

#ifndef OPEN_CRANK_CAN_OTK_H
#define OPEN_CRANK_CAN_OTK_H

#include <stdbool.h>

#include <opencrank/j1939.h>
#include <opentick/can.h>

/** @brief A J1939 frame as a 29-bit classic CAN frame. */
void oc_frame_to_otk(const J1939Frame *in, OtkCanFrame *out);

/**
 * @brief A CAN frame as a J1939 frame. Payload bytes past the length are 0xFF.
 * @return false for frames J1939 does not use: 11-bit identifiers and CAN FD
 */
bool oc_frame_from_otk(const OtkCanFrame *in, J1939Frame *out);

#endif /* OPEN_CRANK_CAN_OTK_H */
