#include "wsse.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ SHA-1 */
/* Public-domain SHA-1 (FIPS 180-1), the classic Steve Reid implementation
 * shape -- self-contained, no libc crypto dependency. */
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
} sha1_ctx_t;

static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t block[80];
    for (int i = 0; i < 16; i++)
        block[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
                   ((uint32_t)buffer[i * 4 + 2] << 8) | (uint32_t)buffer[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        block[i] = rol(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);       k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                  k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                  k = 0xCA62C1D6u; }
        uint32_t temp = rol(a, 5) + f + e + k + block[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count[0] = ctx->count[1] = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t j = (size_t)((ctx->count[0] >> 3) & 63);
    uint32_t bump = (uint32_t)(len << 3);
    if ((ctx->count[0] += bump) < bump)
        ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t i = 0;
    if (len >= 64 - j) {
        memcpy(&ctx->buffer[j], data, 64 - j);
        sha1_transform(ctx->state, ctx->buffer);
        for (i = 64 - j; i + 63 < len; i += 64)
            sha1_transform(ctx->state, data + i);
        j = 0;
    }
    memcpy(&ctx->buffer[j], data + i, len - i);
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++)
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4) ? 0 : 1] >> ((3 - (i & 3)) * 8)) & 0xFF);

    uint8_t c = 0x80;
    sha1_update(ctx, &c, 1);
    uint8_t zero = 0;
    while ((ctx->count[0] & 504) != 448)
        sha1_update(ctx, &zero, 1);
    sha1_update(ctx, finalcount, 8);

    for (int i = 0; i < 20; i++)
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
}

void sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* ------------------------------------------------------- minimal XML scan */
static const char *mem_find(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

typedef struct {
    const char *attrs; size_t attrs_len;     /* raw text between '<Name' and the closing '>' */
    const char *content; size_t content_len; /* text between that '>' and the next '<' */
} xml_elem_t;

/* Finds the next start tag whose local name is exactly `name` (namespace
 * prefix, if any, is ignored -- matches "<foo:Name", "<Name", both followed
 * by '>', ' ' (attributes) or '/' (self-close)). Not a real XML parser: a
 * deliberate, minimal scan consistent with the rest of this daemon's ONVIF
 * handling (see onvif_soap.c's action dispatch). */
static int find_element(const char *body, size_t blen, const char *name, xml_elem_t *out) {
    size_t nlen = strlen(name);
    const char *p = body;
    size_t remaining = blen;
    while (remaining >= nlen) {
        const char *hit = mem_find(p, remaining, name, nlen);
        if (!hit)
            return 0;
        int boundary_ok = (hit > body) && (hit[-1] == ':' || hit[-1] == '<');
        const char *after = hit + nlen;
        int after_ok = (after < body + blen) && (*after == '>' || *after == ' ' || *after == '/');
        if (boundary_ok && after_ok) {
            const char *gt = (const char *)memchr(hit, '>', (size_t)((body + blen) - hit));
            if (gt) {
                out->attrs = hit;
                out->attrs_len = (size_t)(gt - hit);
                const char *content_start = gt + 1;
                const char *lt = (const char *)memchr(content_start, '<',
                                                       (size_t)((body + blen) - content_start));
                out->content = content_start;
                out->content_len = lt ? (size_t)(lt - content_start) : 0;
                return 1;
            }
        }
        size_t skip = (size_t)(hit - p) + 1;
        p += skip;
        remaining -= skip;
    }
    return 0;
}

static void copy_field(const xml_elem_t *e, char *out, size_t out_cap) {
    size_t n = e->content_len < out_cap - 1 ? e->content_len : out_cap - 1;
    memcpy(out, e->content, n);
    out[n] = '\0';
}

int wsse_parse_token(const char *body, size_t blen, wsse_token_t *tok) {
    memset(tok, 0, sizeof *tok);
    xml_elem_t e;
    if (!find_element(body, blen, "UsernameToken", &e))
        return 0; /* no Security header/UsernameToken present at all */
    tok->present = 1;

    if (find_element(body, blen, "Username", &e))
        copy_field(&e, tok->username, sizeof tok->username);
    if (find_element(body, blen, "Password", &e)) {
        copy_field(&e, tok->password, sizeof tok->password);
        tok->password_is_digest = mem_find(e.attrs, e.attrs_len, "PasswordDigest", 14) != NULL;
    }
    if (find_element(body, blen, "Nonce", &e))
        copy_field(&e, tok->nonce_b64, sizeof tok->nonce_b64);
    if (find_element(body, blen, "Created", &e))
        copy_field(&e, tok->created, sizeof tok->created);
    return 1;
}

void wsse_compute_digest(const uint8_t *nonce_raw, size_t nonce_len,
                          const char *created, const char *password,
                          char *out_b64, size_t out_cap) {
    uint8_t buf[256];
    size_t off = 0;
    size_t created_len = strlen(created);
    size_t password_len = strlen(password);

    if (nonce_len > sizeof(buf)) nonce_len = sizeof(buf);
    memcpy(buf + off, nonce_raw, nonce_len);
    off += nonce_len;
    if (off + created_len > sizeof(buf)) created_len = sizeof(buf) - off;
    memcpy(buf + off, created, created_len);
    off += created_len;
    if (off + password_len > sizeof(buf)) password_len = sizeof(buf) - off;
    memcpy(buf + off, password, password_len);
    off += password_len;

    uint8_t digest[20];
    sha1(buf, off, digest);
    char tmp[32];
    b64_encode(digest, sizeof digest, tmp);
    snprintf(out_b64, out_cap, "%s", tmp);
}

int wsse_authenticate(const wsse_token_t *tok, const char *want_user, const char *want_pass) {
    if (!tok->present || !tok->username[0])
        return 0;
    if (strcmp(tok->username, want_user) != 0)
        return 0;

    if (tok->password_is_digest) {
        uint8_t nonce[192];
        size_t nonce_len = b64_decode(tok->nonce_b64, strlen(tok->nonce_b64), nonce, sizeof nonce);
        char expect[32];
        wsse_compute_digest(nonce, nonce_len, tok->created, want_pass, expect, sizeof expect);
        return strcmp(expect, tok->password) == 0;
    }
    /* PasswordText fallback (Type absent or explicitly #PasswordText). */
    return strcmp(tok->password, want_pass) == 0;
}
