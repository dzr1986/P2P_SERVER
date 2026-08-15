// X25519（RFC 7748）
// 域运算与梯子来自 TweetNaCl（公有领域），常量 a24=121665
#include "X25519.h"
#include "Crypto.h"

#include <cstring>

namespace p2p {
namespace {

using i64 = long long;

static void car25519(i64* o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(i64 p[16], i64 q[16], int b) {
    const i64 c = ~(i64)(b - 1);
    for (int i = 0; i < 16; i++) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t* o, const i64* n) {
    i64 t[16], m[16];
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(i64* o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(i64* o, const i64* a, const i64* b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
static void fsub(i64* o, const i64* a, const i64* b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fmul(i64* o, const i64* a, const i64* b) {
    i64 t[31]{};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsqr(i64* o, const i64* a) { fmul(o, a, a); }

static void finv(i64* o, const i64* in) {
    i64 c[16];
    for (int i = 0; i < 16; i++) c[i] = in[i];
    for (int a = 253; a >= 0; a--) {
        fsqr(c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (int i = 0; i < 16; i++) o[i] = c[i];
}

static const i64 k121665[16] = {0xDB41, 1};

} // namespace

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    if (!out || !scalar || !point) return -1;
    uint8_t z[32];
    memcpy(z, scalar, 32);
    z[0] &= 248;
    z[31] = (uint8_t)((z[31] & 127) | 64);

    i64 x[16], a[16]{}, b[16], c[16]{}, d[16]{}, e[16], f[16];
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) b[i] = x[i];
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsqr(d, e);
        fsqr(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsqr(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsqr(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(out, a);
    return 0;
}

int x25519_keypair(uint8_t pub[32], uint8_t priv[32]) {
    if (!pub || !priv) return -1;
    if (p2p_random_bytes(priv, 32) != 0) return -1;
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    uint8_t base[32]{};
    base[0] = 9;
    return x25519(pub, priv, base);
}

} // namespace p2p
