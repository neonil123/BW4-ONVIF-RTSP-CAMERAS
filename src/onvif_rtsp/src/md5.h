/* md5 — RFC 1321 MD5. Self-contained (no libm/libcrypto dependency, table
 * precomputed rather than derived from sin() at runtime), used by
 * httpauth.c for HTTP Digest (RFC 2617) since ONVIF/HTTP Digest mandates
 * MD5, not SHA-1. */
#ifndef CAM_MD5_H
#define CAM_MD5_H

#include <stdint.h>
#include <stddef.h>

void md5(const uint8_t *data, size_t len, uint8_t digest[16]);

#endif /* CAM_MD5_H */
