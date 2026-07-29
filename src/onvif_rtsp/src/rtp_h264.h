/* rtp_h264 — RFC 6184 H.264/RTP packetization. C port of packetize_au() in
 * tools/rtsp_server.py (single-NAL, FU-A fragmentation; STAP-A is not needed
 * because SPS/PPS are injected as separate single-NAL packets ahead of the
 * IDR slice, exactly like the Python reference does).
 */
#ifndef OKAM_RTP_H264_H
#define OKAM_RTP_H264_H

#include <stdint.h>
#include <stddef.h>

#define RTP_PAYLOAD_TYPE 96
#define RTP_CLOCK_HZ     90000u
#define RTP_DEFAULT_MTU  1400u
#define RTP_MAX_PACKET   1500u   /* 12 (RTP hdr) + 2 (FU-A prefix) + mtu, rounded up */

typedef void (*rtp_send_cb)(const uint8_t *pkt, size_t len, void *ctx);

/* Packetize one access unit (nals[0..n_nals)) into RTP packets and hand each
 * one to cb() in a scratch buffer valid only for the callback's duration.
 * *seq is read as the starting sequence number and left as the next one to
 * use (both wrap mod 65536, matching Python's `seq & 0xFFFF`). The marker
 * bit is set on the last packet of the whole access unit, matching
 * packetize_au()'s flatten-then-mark-last behavior. */
void rtp_packetize_au(const uint8_t *const *nals, const size_t *nal_lens, int n_nals,
                       uint16_t *seq, uint32_t ts, uint32_t ssrc, size_t mtu,
                       rtp_send_cb cb, void *ctx);

static inline int rtp_nal_type(const uint8_t *nal, size_t len) {
    return len ? (nal[0] & 0x1F) : 0;
}

#endif /* OKAM_RTP_H264_H */
