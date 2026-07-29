/* vstarcam_frame — C port of tools/vstarcam_frame.py
 *
 * Reassembles the VeePai local media stream (length-prefixed
 * 0xA815AA55 frame container) into complete frames, and splits Annex-B
 * H.264 payloads into NAL units. This is a byte-for-byte port: keep any
 * change mirrored in the Python reference (tests compare the two).
 */
#ifndef CAM_VSTARCAM_FRAME_H
#define CAM_VSTARCAM_FRAME_H

#include <stdint.h>
#include <stddef.h>

#define VSC_MAGIC        0xA815AA55u
#define VSC_HDR_LEN      32u          /* 0x20 */
#define VSC_HDR_SIZE_OFF 0x10u
#define VSC_MAX_PAYLOAD  (4u * 1024u * 1024u)

typedef struct {
    uint8_t        type;
    uint8_t        codec;
    uint32_t       timestamp;
    const uint8_t *payload;      /* points into the reassembler's internal
                                   * buffer; valid only for the duration of
                                   * the vsc_frame_cb call */
    uint32_t       payload_len;
} vsc_frame_t;

typedef void (*vsc_frame_cb)(const vsc_frame_t *f, void *ctx);

typedef struct {
    uint8_t *buf;
    size_t   len;   /* bytes currently buffered */
    size_t   cap;   /* allocated capacity */
} vsc_reassembler_t;

void vsc_init(vsc_reassembler_t *r);
void vsc_reset(vsc_reassembler_t *r);   /* drop buffered bytes, keep allocation */
void vsc_free(vsc_reassembler_t *r);

/* Feed a chunk of raw bytes; invokes cb once per complete frame extracted
 * (may be zero, one, or many calls per vsc_feed()). Mirrors
 * FrameReassembler.feed()/_try_one()/_resync() including magic-resync on a
 * malformed/desynced stream. */
void vsc_feed(vsc_reassembler_t *r, const uint8_t *data, size_t n,
              vsc_frame_cb cb, void *ctx);

/* payload[:4]==00 00 00 01 or payload[:3]==00 00 01 -> looks like Annex-B video. */
int vsc_payload_is_video(const uint8_t *payload, uint32_t len);

typedef void (*vsc_nal_cb)(const uint8_t *nal, size_t len, void *ctx);

/* Split an Annex-B byte stream into NAL units (start code stripped).
 * Mirrors iter_annexb_nals(). */
void vsc_iter_annexb_nals(const uint8_t *data, size_t n, vsc_nal_cb cb, void *ctx);

/* nal[0] & 0x1F */
static inline int vsc_nal_type(const uint8_t *nal, size_t len) {
    return len ? (nal[0] & 0x1F) : 0;
}

#endif /* CAM_VSTARCAM_FRAME_H */
