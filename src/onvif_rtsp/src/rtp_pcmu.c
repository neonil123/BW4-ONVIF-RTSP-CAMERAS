#include "rtp_pcmu.h"
#include <string.h>

/* Write the fixed 12-byte RTP header (mirrors rtp_h264.c's write_hdr, but with
 * PT=0 and the marker bit always clear for continuous audio). */
static void write_hdr(uint8_t *scratch, uint16_t seq, uint32_t ts, uint32_t ssrc) {
    scratch[0] = 0x80;
    scratch[1] = (uint8_t)RTP_PCMU_PAYLOAD_TYPE;   /* marker=0 | PT=0 */
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

void rtp_pcmu_packetize(const uint8_t *mulaw, size_t nbytes,
                        uint16_t *seq, uint32_t *ts, uint32_t ssrc,
                        rtp_pcmu_send_cb cb, void *ctx) {
    uint8_t scratch[12 + RTP_PCMU_SAMPLES_PER_PKT];
    uint16_t s = *seq;
    uint32_t t = *ts;
    size_t off = 0;

    while (off < nbytes) {
        size_t part = nbytes - off;
        if (part > RTP_PCMU_SAMPLES_PER_PKT)
            part = RTP_PCMU_SAMPLES_PER_PKT;
        write_hdr(scratch, s, t, ssrc);
        memcpy(scratch + 12, mulaw + off, part);
        cb(scratch, 12 + part, ctx);
        s = (uint16_t)(s + 1);
        t += (uint32_t)part;   /* PCMU: one 8 kHz sample per byte */
        off += part;
    }

    *seq = s;
    *ts = t;
}
