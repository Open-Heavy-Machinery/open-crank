/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file can_otk.c
 * @brief J1939 frames to and from open-tick's CAN frames. See can_otk.h.
 */

#include <opencrank/can_otk.h>

#include <string.h>

void oc_frame_to_otk(const J1939Frame *in, OtkCanFrame *out) {
    memset(out, 0, sizeof(*out));
    out->id = in->id & OTK_CAN_EFF_MASK;
    out->flags = OTK_CAN_EXTENDED;
    out->len = (in->dlc <= 8U) ? in->dlc : 8U;
    memcpy(out->data, in->data, out->len);
}

bool oc_frame_from_otk(const OtkCanFrame *in, J1939Frame *out) {
    if ((in->flags & OTK_CAN_EXTENDED) == 0U || (in->flags & OTK_CAN_FD) != 0U) {
        return false;
    }
    out->id = in->id & OTK_CAN_EFF_MASK;
    out->dlc = (in->len <= 8U) ? in->len : 8U;
    memset(out->data, 0xFF, sizeof(out->data));
    memcpy(out->data, in->data, out->dlc);
    return true;
}
