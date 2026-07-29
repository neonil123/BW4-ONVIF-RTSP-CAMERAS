#include "httpauth.h"
#include "md5.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static void md5_hex(const uint8_t *data, size_t len, char out_hex[33]) {
    uint8_t d[16];
    md5(data, len, d);
    for (int i = 0; i < 16; i++)
        snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[32] = '\0';
}

void httpauth_init(httpauth_ctx_t *ctx) {
    /* Not cryptographically unpredictable, and deliberately reused for the
     * process's lifetime -- fine for a single-NVR LAN camera (see the
     * header comment); a fresh MD5'd seed just gives a clean fixed-length
     * hex nonce, not a security boundary. */
    char seed[80];
    snprintf(seed, sizeof seed, "okam-onvifd-nonce-%ld-%d", (long)time(NULL), (int)getpid());
    uint8_t d[16];
    md5((const uint8_t *)seed, strlen(seed), d);
    for (int i = 0; i < 16; i++)
        snprintf(ctx->nonce + i * 2, 3, "%02x", d[i]);
    ctx->nonce[32] = '\0';
}

size_t httpauth_challenge_headers(const httpauth_ctx_t *ctx, char *out, size_t out_cap) {
    int n = snprintf(out, out_cap,
                      "WWW-Authenticate: Digest realm=\"%s\", qop=\"auth\", nonce=\"%s\", algorithm=MD5\r\n"
                      "WWW-Authenticate: Basic realm=\"%s\"\r\n",
                      HTTPAUTH_REALM, ctx->nonce, HTTPAUTH_REALM);
    return n > 0 ? (size_t)n : 0;
}

void httpauth_compute_response(const char *user, const char *realm, const char *pass,
                                const char *method, const char *uri,
                                const char *nonce, const char *nc, const char *cnonce,
                                const char *qop, char out_hex[33]) {
    char ha1_in[320];
    snprintf(ha1_in, sizeof ha1_in, "%s:%s:%s", user, realm, pass);
    char ha1[33];
    md5_hex((const uint8_t *)ha1_in, strlen(ha1_in), ha1);

    char ha2_in[320];
    snprintf(ha2_in, sizeof ha2_in, "%s:%s", method, uri);
    char ha2[33];
    md5_hex((const uint8_t *)ha2_in, strlen(ha2_in), ha2);

    char resp_in[512];
    if (qop && qop[0])
        snprintf(resp_in, sizeof resp_in, "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
    else
        snprintf(resp_in, sizeof resp_in, "%s:%s:%s", ha1, nonce, ha2);
    md5_hex((const uint8_t *)resp_in, strlen(resp_in), out_hex);
}

/* Extracts the value of `key=` from a Digest/Basic parameter list (quoted
 * or unquoted), tolerant of the namespace-free "key=value" or key="value""
 * shapes RFC 2617 allows. Not a strict parser: consistent with this
 * daemon's minimal, no-real-parser approach elsewhere (see onvif_soap.c's
 * action dispatch, wsse.c's element scan). Returns 1 if found. */
static int extract_param(const char *s, const char *key, char *out, size_t out_cap) {
    size_t klen = strlen(key);
    const char *p = s;
    while (*p) {
        const char *hit = strstr(p, key);
        if (!hit)
            return 0;
        const char *after = hit + klen;
        int boundary_before = (hit == s) || hit[-1] == ' ' || hit[-1] == ',' || hit[-1] == '\t';
        if (boundary_before && *after == '=') {
            const char *v = after + 1;
            if (*v == '"') {
                v++;
                const char *end = strchr(v, '"');
                if (!end)
                    return 0;
                size_t n = (size_t)(end - v);
                if (n >= out_cap) n = out_cap - 1;
                memcpy(out, v, n);
                out[n] = '\0';
            } else {
                const char *end = v;
                while (*end && *end != ',' && *end != ' ' && *end != '\t')
                    end++;
                size_t n = (size_t)(end - v);
                if (n >= out_cap) n = out_cap - 1;
                memcpy(out, v, n);
                out[n] = '\0';
            }
            return 1;
        }
        p = after; /* not a real match at this position (wrong boundary, or
                    * no '=' right after -- e.g. "nc" hitting inside
                    * "cnonce"); keep scanning past it */
    }
    return 0;
}

int httpauth_validate(const httpauth_ctx_t *ctx, const char *authz, const char *method,
                       const char *want_user, const char *want_pass,
                       char *out_user, size_t out_user_cap,
                       char *out_scheme, size_t out_scheme_cap) {
    if (out_user_cap) out_user[0] = '\0';
    if (out_scheme_cap) out_scheme[0] = '\0';
    if (!authz || !*authz)
        return 0;

    if (!strncasecmp(authz, "Basic ", 6)) {
        if (out_scheme_cap) snprintf(out_scheme, out_scheme_cap, "%s", "Basic");
        const char *b64 = authz + 6;
        while (*b64 == ' ') b64++;
        uint8_t dec[256];
        size_t n = b64_decode(b64, strlen(b64), dec, sizeof dec - 1);
        dec[n] = '\0';
        char *colon = strchr((char *)dec, ':');
        if (!colon)
            return 0;
        *colon = '\0';
        const char *user = (const char *)dec;
        const char *pass = colon + 1;
        if (out_user_cap) snprintf(out_user, out_user_cap, "%s", user);
        return strcmp(user, want_user) == 0 && strcmp(pass, want_pass) == 0;
    }

    if (!strncasecmp(authz, "Digest ", 7)) {
        if (out_scheme_cap) snprintf(out_scheme, out_scheme_cap, "%s", "Digest");
        const char *params = authz + 7;
        char user[64] = "", nonce[64] = "", uri[256] = "", response[64] = "";
        char qop[16] = "", nc[24] = "", cnonce[64] = "";
        extract_param(params, "username", user, sizeof user);
        extract_param(params, "nonce", nonce, sizeof nonce);
        extract_param(params, "uri", uri, sizeof uri);
        extract_param(params, "response", response, sizeof response);
        extract_param(params, "qop", qop, sizeof qop);
        extract_param(params, "nc", nc, sizeof nc);
        extract_param(params, "cnonce", cnonce, sizeof cnonce);
        if (out_user_cap) snprintf(out_user, out_user_cap, "%s", user);

        if (!nonce[0] || strcmp(nonce, ctx->nonce) != 0)
            return 0; /* not a nonce we issued (stale/foreign); client will
                       * get a fresh 401 with our current nonce and retry */
        if (strcmp(user, want_user) != 0)
            return 0;

        char expect[33];
        httpauth_compute_response(want_user, HTTPAUTH_REALM, want_pass, method, uri,
                                   nonce, nc, cnonce, qop, expect);
        return strcasecmp(expect, response) == 0;
    }

    return 0;
}
