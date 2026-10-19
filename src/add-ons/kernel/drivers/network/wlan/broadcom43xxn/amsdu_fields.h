/* SPDX-License-Identifier: MIT */
#ifndef RENKU_AMSDU_FIELDS_H
#define RENKU_AMSDU_FIELDS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct renku_msdu_input { const uint8_t* bytes; size_t length; };
/* Inputs are unsent QoS ToDS MPDUs before IV/PN allocation. Caller restricts
 * these to pure TCP ACKs owned by the same node. No payload is discarded. */
static size_t
renku_amsdu_pack(const struct renku_msdu_input* in, unsigned count,
    uint8_t* out, size_t capacity)
{
    if (in == NULL || out == NULL || count < 2 || count > 24) return 0;
    size_t total = 26;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t* f = in[i].bytes;
        if (f == NULL || in[i].length < 34 || in[i].length > 192
            || f[0] != 0x88 || f[1] != 0x41 || (f[4] & 1) != 0
            || (f[22] & 15) != 0 || (f[24] & 0xf0) != 0 || f[25] != 0)
            return 0;
        if (i != 0 && (memcmp(f + 4, in[0].bytes + 4, 12) != 0
            || f[24] != in[0].bytes[24])) return 0;
        size_t sub = 14 + in[i].length - 26;
        total += i + 1 == count ? sub : (sub + 3) & ~(size_t)3;
    }
    if (total > capacity || total > 2000) return 0;
    memcpy(out, in[0].bytes, 26);
    out[24] |= 0x80;
    size_t pos = 26;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t* f = in[i].bytes;
        size_t payload = in[i].length - 26;
        memcpy(out + pos, f + 16, 6); // Ethernet destination, not AP RA.
        memcpy(out + pos + 6, f + 10, 6);
        out[pos + 12] = payload >> 8;
        out[pos + 13] = payload & 255;
        memcpy(out + pos + 14, f + 26, payload);
        size_t sub = 14 + payload;
        size_t aligned = i + 1 == count ? sub : (sub + 3) & ~(size_t)3;
        memset(out + pos + sub, 0, aligned - sub);
        pos += aligned;
    }
    return total;
}
#endif
