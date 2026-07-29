/* vstarcam_frame — see header. Byte-for-byte port of tools/vstarcam_frame.py. */
#include "vstarcam_frame.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t MAGIC_LE[4] = { 0x55, 0xAA, 0x15, 0xA8 }; /* struct.pack("<I", MAGIC) */

void vsc_init(vsc_reassembler_t *r) {
    r->buf = NULL;
    r->len = 0;
    r->cap = 0;
}

void vsc_reset(vsc_reassembler_t *r) {
    r->len = 0;
}

void vsc_free(vsc_reassembler_t *r) {
    free(r->buf);
    r->buf = NULL;
    r->len = r->cap = 0;
}

static void ensure_cap(vsc_reassembler_t *r, size_t need) {
    if (need <= r->cap)
        return;
    size_t newcap = r->cap ? r->cap * 2 : 4096;
    while (newcap < need)
        newcap *= 2;
    r->buf = (uint8_t *)realloc(r->buf, newcap);
    r->cap = newcap;
}

/* Drop `n` bytes from the front, shifting the remainder down. */
static void drop_front(vsc_reassembler_t *r, size_t n) {
    if (n >= r->len) {
        r->len = 0;
        return;
    }
    memmove(r->buf, r->buf + n, r->len - n);
    r->len -= n;
}

/* Mirrors _resync(): drop bytes up to the next MAGIC found at index >= 1.
 * Returns 1 if a magic remains buffered, 0 otherwise (in which case only a
 * possible partial-magic tail (<=3 bytes) is kept, matching the Python). */
static int resync(vsc_reassembler_t *r) {
    size_t i;
    if (r->len < 1) {
        r->len = 0;
        return 0;
    }
    for (i = 1; i + 4 <= r->len; i++) {
        if (memcmp(r->buf + i, MAGIC_LE, 4) == 0) {
            drop_front(r, i);
            return 1;
        }
    }
    /* no magic found: keep only a possible partial magic at the tail */
    size_t keep = r->len < 3 ? r->len : 3;
    drop_front(r, r->len - keep);
    return 0;
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Mirrors _try_one(), looped until it needs more data. */
static void try_all(vsc_reassembler_t *r, vsc_frame_cb cb, void *ctx) {
    for (;;) {
        if (r->len < VSC_HDR_LEN)
            return;
        if (memcmp(r->buf, MAGIC_LE, 4) != 0) {
            if (!resync(r))
                return;
            continue;
        }
        uint8_t  typ   = r->buf[0x04];
        uint8_t  codec = r->buf[0x05];
        uint32_t ts    = rd_le32(r->buf + 0x0C);
        uint32_t size  = rd_le32(r->buf + VSC_HDR_SIZE_OFF);

        if (size > VSC_MAX_PAYLOAD) {
            /* bogus length -> skip this magic (4 bytes) and resync */
            drop_front(r, 4);
            if (!resync(r))
                return;
            continue;
        }
        size_t total = VSC_HDR_LEN + (size_t)size;
        if (r->len < total)
            return; /* wait for more */

        vsc_frame_t f;
        f.type = typ;
        f.codec = codec;
        f.timestamp = ts;
        f.payload = r->buf + VSC_HDR_LEN;
        f.payload_len = size;
        cb(&f, ctx);

        drop_front(r, total);
        /* loop: more frames may already be fully buffered */
    }
}

void vsc_feed(vsc_reassembler_t *r, const uint8_t *data, size_t n,
              vsc_frame_cb cb, void *ctx) {
    if (n) {
        ensure_cap(r, r->len + n);
        memcpy(r->buf + r->len, data, n);
        r->len += n;
    }
    try_all(r, cb, ctx);
}

int vsc_payload_is_video(const uint8_t *payload, uint32_t len) {
    if (len >= 4 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 && payload[3] == 1)
        return 1;
    if (len >= 3 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1)
        return 1;
    return 0;
}

void vsc_iter_annexb_nals(const uint8_t *data, size_t n, vsc_nal_cb cb, void *ctx) {
    /* Mirrors iter_annexb_nals(): find every start-code position, then slice
     * each NAL as [start_of_next_start_code_or_end). */
    if (n < 3)
        return;

    size_t cap = 64, cnt = 0;
    size_t *pos = (size_t *)malloc(cap * sizeof(size_t));
    size_t *sclen = (size_t *)malloc(cap * sizeof(size_t));

    size_t i = 0;
    while (i + 2 < n) { /* i < n - 2 */
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                if (cnt == cap) {
                    cap *= 2;
                    pos = (size_t *)realloc(pos, cap * sizeof(size_t));
                    sclen = (size_t *)realloc(sclen, cap * sizeof(size_t));
                }
                pos[cnt] = i; sclen[cnt] = 3; cnt++;
                i += 3;
                continue;
            }
            if (i + 3 < n && data[i + 2] == 0 && data[i + 3] == 1) {
                if (cnt == cap) {
                    cap *= 2;
                    pos = (size_t *)realloc(pos, cap * sizeof(size_t));
                    sclen = (size_t *)realloc(sclen, cap * sizeof(size_t));
                }
                pos[cnt] = i; sclen[cnt] = 4; cnt++;
                i += 4;
                continue;
            }
        }
        i++;
    }

    for (size_t k = 0; k < cnt; k++) {
        size_t begin = pos[k] + sclen[k];
        size_t end = (k + 1 < cnt) ? pos[k + 1] : n;
        if (end > begin)
            cb(data + begin, end - begin, ctx);
    }

    free(pos);
    free(sclen);
}
