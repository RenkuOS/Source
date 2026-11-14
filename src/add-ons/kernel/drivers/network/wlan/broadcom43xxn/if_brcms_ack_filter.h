/* SPDX-License-Identifier: MIT */
/* Copyright 2026, rainygirl <rainygirl@gmail.com>. */
#ifndef IF_BRCMS_ACK_FILTER_H
#define IF_BRCMS_ACK_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct brcms_ack_key {
  uint8_t addresses[18];
  uint8_t hosts[8];
  uint8_t ports[4];
  uint32_t sequence, acknowledgment, timestamp, echo;
  uint16_t window;
  uint8_t control, tid, tos, ttl;
  bool eligible, timestamps;
};

static inline uint32_t brcms_ack_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}

/* Parse a flow even if it is ineligible: a later data/SACK/window update
 * in that flow must prevent replacement of an earlier acknowledgment. */
static inline bool brcms_ack_parse(const uint8_t *bytes, size_t copied,
                                   size_t length, size_t headerLength,
                                   struct brcms_ack_key *key) {
  static const uint8_t snap[] = {0xaa, 0xaa, 3, 0, 0, 0, 8, 0};
  if ((headerLength != 26 && headerLength != 28) ||
      copied < headerLength + 8 + 20 || length < copied || bytes[0] != 0x88 ||
      (bytes[1] & 0x87) != 1 ||
      memcmp(bytes + headerLength, snap, sizeof(snap)) != 0)
    return false;
  const uint8_t *ip = bytes + headerLength + 8;
  size_t ipLength = (ip[0] & 15) * 4;
  size_t total = (unsigned)ip[2] << 8 | ip[3];
  if ((ip[0] >> 4) != 4 || ipLength < 20 || ip[9] != 6 || (ip[6] & 0xbf) != 0 ||
      ip[7] != 0 || total < ipLength + 20 ||
      headerLength + 8 + total != length ||
      copied < headerLength + 8 + ipLength + 20)
    return false;
  const uint8_t *tcp = ip + ipLength;
  size_t tcpLength = (tcp[12] >> 4) * 4;
  if (tcpLength < 20 || total < ipLength + tcpLength)
    return false;
  memset(key, 0, sizeof(*key));
  memcpy(key->addresses, bytes + 4, sizeof(key->addresses));
  memcpy(key->hosts, ip + 12, sizeof(key->hosts));
  memcpy(key->ports, tcp, sizeof(key->ports));
  key->control = bytes[1];
	key->tid = bytes[24] & 15;
  key->tos = ip[1];
  key->ttl = ip[8];
  key->sequence = brcms_ack_be32(tcp + 4);
  key->acknowledgment = brcms_ack_be32(tcp + 8);
  key->window = (unsigned)tcp[14] << 8 | tcp[15];
  if (ipLength != 20 || total != ipLength + tcpLength || tcp[13] != 0x10 ||
      (tcp[12] & 15) != 0 || tcp[18] != 0 || tcp[19] != 0 || key->window == 0 ||
      (bytes[24] & 0xf0) != 0 || bytes[25] != 0 || (bytes[22] & 15) != 0 ||
      (bytes[1] & 0x18) != 0)
    return true;
  if (tcpLength == 32) {
    if (copied < headerLength + 8 + ipLength + tcpLength || tcp[20] != 1 ||
        tcp[21] != 1 || tcp[22] != 8 || tcp[23] != 10)
      return true;
    key->timestamps = true;
    key->timestamp = brcms_ack_be32(tcp + 24);
    key->echo = brcms_ack_be32(tcp + 28);
  } else if (tcpLength != 20)
    return true;
  key->eligible = true;
  return true;
}

static inline bool brcms_ack_same_flow(const struct brcms_ack_key *a,
                                       const struct brcms_ack_key *b) {
  return a->tid == b->tid &&
         memcmp(a->addresses, b->addresses, sizeof(a->addresses)) == 0 &&
         memcmp(a->hosts, b->hosts, sizeof(a->hosts)) == 0 &&
         memcmp(a->ports, b->ports, sizeof(a->ports)) == 0;
}

static inline bool brcms_ack_supersedes(const struct brcms_ack_key *newer,
                                        const struct brcms_ack_key *older) {
  uint32_t advance = newer->acknowledgment - older->acknowledgment;
  return newer->eligible && older->eligible &&
         brcms_ack_same_flow(newer, older) &&
         // Both packets are still unsent. Preserve the latest cumulative
         // ACK and its actual window, including a smaller nonzero window.
         // Do not infer a scaled right edge or rewrite TCP state. Zero
         // windows, duplicate ACKs and recovery/control packets stay intact.
         newer->sequence == older->sequence && newer->window != 0 &&
         older->window != 0 &&
         newer->control == older->control && newer->tos == older->tos &&
         newer->ttl == older->ttl && newer->timestamps == older->timestamps &&
         advance != 0 && advance < 0x80000000U &&
         (!newer->timestamps ||
          (newer->timestamp - older->timestamp < 0x80000000U &&
           newer->echo - older->echo < 0x80000000U));
}

#endif
