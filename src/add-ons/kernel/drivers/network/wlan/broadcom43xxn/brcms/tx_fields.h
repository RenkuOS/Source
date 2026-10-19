/* SPDX-License-Identifier: MIT */
#ifndef BRCMS_CCMP_TX_FIELDS_H
#define BRCMS_CCMP_TX_FIELDS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
struct brcms_ccmp_fields { uint16_t security; uint8_t iv[8]; };
/* Caller separately proves ownership of the programmed key. Input contains
 * plaintext plus CCMP IV, but no MIC/FCS. No PN allocation occurs here. */
static bool
brcms_ccmp_prepare_fields(unsigned slot, const uint8_t* frame, size_t length,
    struct brcms_ccmp_fields* fields)
{
    if (slot < 4 || slot >= 54 || frame == NULL || fields == NULL
        || length < 32 || length > 2040)
        return false;
    if ((frame[0] & 15) != 8 || (frame[1] & 0x40) == 0
        || (frame[1] & 3) != 1 || (frame[4] & 1) != 0)
        return false;
    size_t header = 24;
    if ((frame[0] & 0x80) != 0) {
        header += 2;
        if ((frame[1] & 0x80) != 0) header += 4;
    }
    if (length < header + 8) return false;
    const uint8_t* iv = frame + header;
    if (iv[2] != 0 || iv[3] != 0x20
        || (iv[0] | iv[1] | iv[4] | iv[5] | iv[6] | iv[7]) == 0)
        return false;
    fields->security = (uint16_t)((slot << 4) | 3);
    memcpy(fields->iv, iv, 8);
    return true;
}
#endif
