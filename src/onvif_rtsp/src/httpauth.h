/* httpauth — HTTP Basic (RFC 7617) and HTTP Digest (RFC 2617) authentication
 * for the ONVIF SOAP endpoint. Most NVRs (Synology Surveillance Station
 * included) authenticate ONVIF at the HTTP transport layer rather than via
 * a WS-Security UsernameToken: an unauthenticated request must get back
 * HTTP 401 with a WWW-Authenticate challenge, and the client retries with
 * an Authorization header. onvif_soap.c accepts either this OR a valid
 * WS-Security UsernameToken (see wsse.c) -- any one authenticates.
 */
#ifndef OKAM_HTTPAUTH_H
#define OKAM_HTTPAUTH_H

#include <stddef.h>

#define HTTPAUTH_REALM "okam_onvifd"

typedef struct {
    char nonce[40]; /* generated once at startup, reused for the process's
                     * lifetime -- "tolerate reuse" is fine here per the
                     * single-NVR LAN use case; see httpauth_init(). */
} httpauth_ctx_t;

void httpauth_init(httpauth_ctx_t *ctx);

/* Writes the two WWW-Authenticate challenge header lines (Digest, then
 * Basic, each ending \r\n) into `out`. Returns the length written
 * (excluding the NUL terminator this also writes). */
size_t httpauth_challenge_headers(const httpauth_ctx_t *ctx, char *out, size_t out_cap);

/* The RFC 2617 digest formula, exposed standalone for testability:
 *   HA1 = MD5(user:realm:pass)
 *   HA2 = MD5(method:uri)
 *   response = MD5(HA1:nonce:nc:cnonce:qop:HA2)      if qop is non-empty
 *            = MD5(HA1:nonce:HA2)                     if qop is empty ("")
 * Writes the 32-hex-char response (+ NUL) into out_hex (>=33 bytes). */
void httpauth_compute_response(const char *user, const char *realm, const char *pass,
                                const char *method, const char *uri,
                                const char *nonce, const char *nc, const char *cnonce,
                                const char *qop, char out_hex[33]);

/* Validates an "Authorization: ..." header VALUE (i.e. the header's value,
 * without the "Authorization:" name) against want_user/want_pass. `method`
 * is the HTTP request method (e.g. "POST"). On return, out_scheme is set
 * to "Basic", "Digest", or "" (unrecognized/absent), and out_user to the
 * username it parsed out (empty if none). Returns 1 if it authenticates. */
int httpauth_validate(const httpauth_ctx_t *ctx, const char *authz_header, const char *method,
                       const char *want_user, const char *want_pass,
                       char *out_user, size_t out_user_cap,
                       char *out_scheme, size_t out_scheme_cap);

#endif /* OKAM_HTTPAUTH_H */
