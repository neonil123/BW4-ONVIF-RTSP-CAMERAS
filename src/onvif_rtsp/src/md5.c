#include "md5.h"
#include <string.h>
#include <stdlib.h>

/* Per-round left-rotate amounts (RFC 1321 / Wikipedia's single-loop form). */
static const int S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,
};

/* K[i] = floor(abs(sin(i+1)) * 2^32), precomputed (no libm dependency). */
static const uint32_t K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5(const uint8_t *data, size_t len, uint8_t digest[16]) {
    uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;

    uint64_t bitlen = (uint64_t)len * 8;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = (uint8_t *)calloc(1, padded_len);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++)
        msg[padded_len - 8 + i] = (uint8_t)((bitlen >> (8 * i)) & 0xFF);

    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = msg + chunk + (size_t)i * 4;
            M[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }

        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16)      { f = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { f = (D & B) | (~D & C);        g = (5 * i + 1) % 16; }
            else if (i < 48) { f = B ^ C ^ D;                 g = (3 * i + 5) % 16; }
            else             { f = C ^ (B | (~D));            g = (7 * i) % 16; }
            f = f + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + rotl(f, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    free(msg);

    uint32_t vals[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++) {
        digest[i * 4 + 0] = (uint8_t)(vals[i] & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((vals[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((vals[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)((vals[i] >> 24) & 0xFF);
    }
}
