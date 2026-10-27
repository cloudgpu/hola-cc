/*
 * chacha20.h — ChaCha20 (RFC 8439) stream cipher, used to encrypt the
 * strings of a compiled program (the compiler encrypts, the runtime
 * decrypts at startup). Header-only so both sides share one copy.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#ifndef HL_CHACHA20_H
#define HL_CHACHA20_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HL_CC_ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define HL_CC_QR(a, b, c, d)                                   \
    (a += b, d ^= a, d = HL_CC_ROTL(d, 16), c += d, b ^= c, b = HL_CC_ROTL(b, 12), \
     a += b, d ^= a, d = HL_CC_ROTL(d, 8), c += d, b ^= c, b = HL_CC_ROTL(b, 7))

static inline uint32_t hl_cc_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void hl_cc_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = hl_cc_le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = hl_cc_le32(nonce + 4 * i);
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        HL_CC_QR(x[0], x[4], x[8], x[12]); HL_CC_QR(x[1], x[5], x[9], x[13]);
        HL_CC_QR(x[2], x[6], x[10], x[14]); HL_CC_QR(x[3], x[7], x[11], x[15]);
        HL_CC_QR(x[0], x[5], x[10], x[15]); HL_CC_QR(x[1], x[6], x[11], x[12]);
        HL_CC_QR(x[2], x[7], x[8], x[13]); HL_CC_QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4 * i] = (uint8_t)v; out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16); out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

/* XORs `n` bytes of `buf` with the keystream for (key, nonce). Encrypts and decrypts. */
static inline void hl_chacha20(const uint8_t key[32], const uint8_t nonce[12], uint8_t *buf, size_t n) {
    uint8_t ks[64];
    for (uint32_t ctr = 0; n; ctr++) {
        hl_cc_block(key, ctr, nonce, ks);
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++) buf[i] ^= ks[i];
        buf += k;
        n -= k;
    }
}

/* The nonce of string number `i` in a program. */
static inline void hl_cc_nonce(uint64_t i, uint8_t nonce[12]) {
    memset(nonce, 0, 12);
    for (int b = 0; b < 8; b++) nonce[4 + b] = (uint8_t)(i >> (8 * b));
}

#endif
