#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap) {
    size_t o = 0;
    int group[4];
    int n = 0, pad = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        int v;
        if (c == '=') { v = 0; pad++; }
        else {
            v = b64_val(c);
            if (v < 0) continue; /* skip anything unexpected rather than error out */
        }
        group[n++] = v;
        if (n == 4) {
            uint32_t triple = ((uint32_t)group[0] << 18) | ((uint32_t)group[1] << 12) |
                               ((uint32_t)group[2] << 6) | (uint32_t)group[3];
            int nbytes = 3 - (pad > 2 ? 2 : pad);
            uint8_t bytes[3] = {
                (uint8_t)((triple >> 16) & 0xFF),
                (uint8_t)((triple >> 8) & 0xFF),
                (uint8_t)(triple & 0xFF),
            };
            for (int k = 0; k < nbytes && o < out_cap; k++)
                out[o++] = bytes[k];
            n = 0;
            pad = 0;
        }
    }
    return o;
}

void gen_uuid(char *out) {
    static int seeded = 0;
    if (!seeded) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        srand((unsigned)(tv.tv_sec ^ tv.tv_usec ^ getpid()));
        seeded = 1;
    }
    unsigned char b[16];
    for (int i = 0; i < 16; i++)
        b[i] = (unsigned char)(rand() & 0xFF);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80); /* variant */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void local_ip_for(struct in_addr dst, char *out) {
    strcpy(out, "0.0.0.0");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(3702);
    sa.sin_addr = dst;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        struct sockaddr_in local;
        socklen_t sl = sizeof(local);
        if (getsockname(s, (struct sockaddr *)&local, &sl) == 0)
            inet_ntop(AF_INET, &local.sin_addr, out, 16);
    }
    close(s);
}

void local_ip_of_fd(int fd, char *out) {
    strcpy(out, "0.0.0.0");
    struct sockaddr_in local;
    socklen_t sl = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &sl) == 0)
        inet_ntop(AF_INET, &local.sin_addr, out, 16);
}

static int g_log_level = LOG_INFO;

void log_set_level(int level) { g_log_level = level; }

void log_msg(int level, const char *fmt, ...) {
    static const char *names[] = { "ERR", "WARN", "INFO", "DEBUG" };
    if (level > g_log_level)
        return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %-5s ",
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long)(tv.tv_usec / 1000),
            names[level < 0 || level > 3 ? 0 : level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void sleep_seconds(double s) {
    if (s <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}
