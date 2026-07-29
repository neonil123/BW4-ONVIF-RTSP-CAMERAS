/* wsse — ONVIF WS-Security UsernameToken (PasswordDigest) authentication.
 *
 * ONVIF Profile S requires WS-UsernameToken auth on the Device/Media SOAP
 * services (GetSystemDateAndTime/GetHostname/GetWsdlUrl and WS-Discovery are
 * the spec's carve-outs, so a client can sync its clock and discover before
 * it has credentials). No XML parser: like onvif_soap.c's action dispatch,
 * the wsse:Security header is located and its fields extracted by scanning
 * for element names, tolerant of the namespace prefix used.
 */
#ifndef OKAM_WSSE_H
#define OKAM_WSSE_H

#include <stdint.h>
#include <stddef.h>

/* SHA-1 (FIPS 180-1). Self-contained -- no libc/OpenSSL dependency, so the
 * static build gains no new NEEDED libs. `digest` must be >= 20 bytes. */
void sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

typedef struct {
    int  present;              /* a wsse:UsernameToken was found at all */
    char username[64];
    char password[128];        /* raw text as sent (digest is base64, or plaintext) */
    int  password_is_digest;   /* Password Type ends in "#PasswordDigest" */
    char nonce_b64[128];       /* raw base64 as sent (Nonce is base64-encoded) */
    char created[64];          /* raw wsu:Created text, not required to be fresh */
} wsse_token_t;

/* Scans `body` for a wsse:UsernameToken and fills `tok`. Returns 1 if a
 * UsernameToken element was found (tok->present is also set to that same
 * value -- the return value exists so callers don't have to separately
 * check tok->present), 0 if none was found at all (tok is still
 * zero-initialized in that case, safe to pass to wsse_authenticate). */
int wsse_parse_token(const char *body, size_t blen, wsse_token_t *tok);

/* WS-UsernameToken Profile 1.0 digest: Base64(SHA1(Nonce || Created || Password)),
 * where Nonce is the RAW (decoded) nonce bytes and Created/Password are the
 * exact UTF-8 octets of those strings, concatenated with no separators.
 * Writes the base64 digest into out_b64 (>=32 bytes covers a 20-byte SHA1). */
void wsse_compute_digest(const uint8_t *nonce_raw, size_t nonce_len,
                          const char *created, const char *password,
                          char *out_b64, size_t out_cap);

/* Authenticates `tok` against the configured username/password. Accepts
 * PasswordDigest (recomputes and compares) and, as a fallback, PasswordText
 * (plaintext compare) -- both per the WS-UsernameToken profile. Does NOT
 * enforce any Created-timestamp freshness window (the camera's RTC may be
 * skewed; the digest itself is what authenticates). Returns 1 on success. */
int wsse_authenticate(const wsse_token_t *tok, const char *want_user, const char *want_pass);

#endif /* OKAM_WSSE_H */
