#include "rtp_h264.h"
#include <string.h>

/* Number of RTP payloads one NAL expands to (1 if it fits under mtu, else the
 * FU-A fragment count) -- mirrors packetize_au()'s inner loop enough to let
 * us compute "is this the last payload of the AU" in a single pass without
 * building the Python-side intermediate `payloads` list. */
static int nal_payload_count(size_t nal_len, size_t mtu) {
    if (nal_len <= mtu)
        return 1;
    size_t body_len = nal_len - 1;      /* NAL header byte consumed by FU indicator/header */
    size_t chunk = mtu - 2;             /* FU indicator + FU header take 2 bytes of the MTU */
    size_t frags = (body_len + chunk - 1) / chunk;
    return frags ? (int)frags : 1;
}

/* Write the fixed 12-byte RTP header into scratch[0..11]. */
static void write_hdr(uint8_t *scratch, uint16_t seq, uint32_t ts, uint32_t ssrc, int marker) {
    scratch[0] = 0x80;
    scratch[1] = (uint8_t)(((marker ? 1 : 0) << 7) | RTP_PAYLOAD_TYPE);
    scratch[2] = (uint8_t)(seq >> 8);
    scratch[3] = (uint8_t)(seq & 0xFF);
    scratch[4] = (uint8_t)(ts >> 24);
    scratch[5] = (uint8_t)(ts >> 16);
    scratch[6] = (uint8_t)(ts >> 8);
    scratch[7] = (uint8_t)(ts & 0xFF);
    scratch[8] = (uint8_t)(ssrc >> 24);
    scratch[9] = (uint8_t)(ssrc >> 16);
    scratch[10] = (uint8_t)(ssrc >> 8);
    scratch[11] = (uint8_t)(ssrc & 0xFF);
}

static void emit(uint8_t *scratch, const uint8_t *payload, size_t plen,
                  uint16_t seq, uint32_t ts, uint32_t ssrc, int marker,
                  rtp_send_cb cb, void *ctx) {
    write_hdr(scratch, seq, ts, ssrc, marker);
    if (plen)
        memcpy(scratch + 12, payload, plen);
    cb(scratch, 12 + plen, ctx);
}

void rtp_packetize_au(const uint8_t *const *nals, const size_t *nal_lens, int n_nals,
                       uint16_t *seq, uint32_t ts, uint32_t ssrc, size_t mtu,
                       rtp_send_cb cb, void *ctx) {
    if (mtu == 0)
        mtu = RTP_DEFAULT_MTU;

    /* Pass 1: total payload count across the whole AU, to know which single
     * packet (across all NALs) gets the marker bit -- matches Python's
     * flatten-then-mark-only-the-last-element behavior exactly. */
    int total = 0;
    for (int i = 0; i < n_nals; i++)
        total += nal_payload_count(nal_lens[i], mtu);
    if (total <= 0)
        return;

    uint8_t scratch[RTP_MAX_PACKET];
    uint16_t s = *seq;
    int idx = 0;

    for (int i = 0; i < n_nals; i++) {
        const uint8_t *nal = nals[i];
        size_t len = nal_lens[i];
        if (len == 0)
            continue;

        if (len <= mtu) {
            int marker = (idx == total - 1);
            emit(scratch, nal, len, s, ts, ssrc, marker, cb, ctx);
            s = (uint16_t)(s + 1);
            idx++;
            continue;
        }

        /* FU-A fragmentation */
        uint8_t nri = nal[0] & 0x60;
        uint8_t typ = nal[0] & 0x1F;
        const uint8_t *body = nal + 1;
        size_t body_len = len - 1;
        size_t chunk = mtu - 2;
        size_t off = 0;
        int first = 1;
        while (off < body_len) {
            size_t part = (body_len - off < chunk) ? (body_len - off) : chunk;
            int last = (off + part >= body_len);
            uint8_t fu_ind = (uint8_t)(nri | 28);
            uint8_t fu_hdr = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | typ);
            int marker = (idx == total - 1);
            write_hdr(scratch, s, ts, ssrc, marker);
            scratch[12] = fu_ind;
            scratch[13] = fu_hdr;
            memcpy(scratch + 14, body + off, part);
            cb(scratch, 12 + 2 + part, ctx);

            s = (uint16_t)(s + 1);
            idx++;
            off += part;
            first = 0;
        }
    }

    *seq = s;
}
