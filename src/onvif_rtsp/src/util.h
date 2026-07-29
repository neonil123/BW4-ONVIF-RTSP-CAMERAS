/* Small shared helpers: base64, local-IP discovery, id/uuid generation,
 * monotonic time, and leveled logging to stderr (line-buffered, matches the
 * Python proxy's logging so serial/log capture on-device looks familiar). */
#ifndef OKAM_UTIL_H
#define OKAM_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

/* Encodes `len` bytes of `in` into base64 (no line breaks) into `out`
 * (caller-provided, must be >= b64_encoded_len(len)+1 bytes). Returns the
 * encoded length (excluding the NUL terminator this also writes). */
size_t b64_encode(const uint8_t *in, size_t len, char *out);
static inline size_t b64_encoded_len(size_t len) { return ((len + 2) / 3) * 4; }

/* Decodes base64 text (whitespace tolerated, '=' padding tolerated/ignored,
 * any other invalid character is skipped defensively rather than treated as
 * an error -- this is only used to decode WS-Security Nonce values, which
 * are always well-formed in practice). Writes up to out_cap bytes into
 * `out` and returns the number of bytes written. */
size_t b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap);

/* Fills `out` (>=37 bytes) with a random-looking lowercase-hex UUID string
 * (8-4-4-4-12). Not cryptographically strong -- only used for WS-Discovery
 * MessageIDs / RTSP session ids, where uniqueness (not unpredictability)
 * is what matters. */
void gen_uuid(char *out);

/* Discovers the local IPv4 address this host would use to reach `dst` by
 * connect()-ing a throwaway UDP socket (no packet is actually sent) and
 * reading getsockname(). Falls back to INADDR_ANY (0.0.0.0) on failure.
 * Used for WS-Discovery ProbeMatch XAddrs, where the reply must advertise
 * the address valid on the interface the Probe arrived on. */
void local_ip_for(struct in_addr dst, char *out /* >=16 bytes */);

/* getsockname() on an already-connected/accepted socket -> dotted string.
 * This is the preferred way to learn "our IP as seen by this client" for a
 * TCP-based service (RTSP/ONVIF SOAP): it is exact, no heuristics needed. */
void local_ip_of_fd(int fd, char *out /* >=16 bytes */);

enum { LOG_ERR = 0, LOG_WARN, LOG_INFO, LOG_DEBUG };
void log_set_level(int level);
void log_msg(int level, const char *fmt, ...);

#define LOGE(...) log_msg(LOG_ERR,  __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)

double monotonic_seconds(void);
void sleep_seconds(double s);

#endif /* OKAM_UTIL_H */
