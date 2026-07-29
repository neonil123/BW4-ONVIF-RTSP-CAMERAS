/* rtp_pcmu — RFC 3551 G.711 PCMU/RTP packetization. The audio-track sibling of
 * rtp_h264.c: static payload type 0, 8000 Hz clock, 1 sample == 1 byte == one
 * timestamp tick. Callback-based send (identical shape to rtp_h264's
 * rtp_send_cb) so the RTSP layer stays transport-agnostic.
 */
#ifndef OKAM_RTP_PCMU_H
#define OKAM_RTP_PCMU_H

#include <stdint.h>
#include <stddef.h>

#define RTP_PCMU_PAYLOAD_TYPE   0
#define RTP_PCMU_CLOCK_HZ       8000u
#define RTP_PCMU_SAMPLES_PER_PKT 160   /* 20 ms @ 8 kHz */

typedef void (*rtp_pcmu_send_cb)(const uint8_t *pkt, size_t len, void *ctx);

/* Packetize `nbytes` of mu-law samples into one or more RTP packets of at most
 * RTP_PCMU_SAMPLES_PER_PKT bytes each and hand each to cb() in a scratch buffer
 * valid only for the callback's duration. *seq and *ts are read as the current
 * sequence number / timestamp and advanced (seq wraps mod 65536; ts += bytes
 * sent, since PCMU is one sample per byte). The marker bit is never set on a
 * plain continuous audio stream. */
void rtp_pcmu_packetize(const uint8_t *mulaw, size_t nbytes,
                        uint16_t *seq, uint32_t *ts, uint32_t ssrc,
                        rtp_pcmu_send_cb cb, void *ctx);

#endif /* OKAM_RTP_PCMU_H */
