#include "core/foundation/Crypto.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/random.h>
#include <unistd.h>

namespace p2p {

namespace {

// ---- GF(2^8) 运算与 AES 查表（运行时自建，避免手工抄表出错） ----
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return r;
}

inline uint8_t ginv(uint8_t a) {
    // a^254（费马小定理）
    uint8_t res = 1, base = a;
    uint32_t e = 254;
    while (e) {
        if (e & 1) res = gmul(res, base);
        base = gmul(base, base);
        e >>= 1;
    }
    return res;
}

inline uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

struct AesTables {
    uint8_t sbox[256];
    uint8_t inv_sbox[256];
    // 预计算 GF(2^8) 乘法表（AES MixColumns/InvMixColumns 只用到 2/3/9/11/13/14）
    uint8_t mul2[256];
    uint8_t mul3[256];
    uint8_t mul9[256];
    uint8_t mul11[256];
    uint8_t mul13[256];
    uint8_t mul14[256];
    AesTables() {
        for (int i = 0; i < 256; i++) {
            uint8_t x = (i == 0) ? 0 : ginv((uint8_t)i);
            uint8_t s = x ^ rotl8(x, 1) ^ rotl8(x, 2) ^ rotl8(x, 3) ^ rotl8(x, 4) ^ 0x63u;
            sbox[i] = s;
            inv_sbox[s] = (uint8_t)i;
            // 预计算常用乘法表
            mul2[i]  = gmul((uint8_t)i, 2);
            mul3[i]  = gmul((uint8_t)i, 3);
            mul9[i]  = gmul((uint8_t)i, 9);
            mul11[i] = gmul((uint8_t)i, 11);
            mul13[i] = gmul((uint8_t)i, 13);
            mul14[i] = gmul((uint8_t)i, 14);
        }
    }
};
const AesTables AES;

constexpr int AES_BLOCK = 16;
constexpr int AES_ROUNDS = 14;          // AES-256
constexpr int AES_RK_WORDS = 60;        // 4*(rounds+1)

// rcon[1..10]
const uint8_t RCON[11] = {0, 0x01, 0x02, 0x04, 0x08, 0x10,
                          0x20, 0x40, 0x80, 0x1b, 0x36};

inline uint8_t rk_byte(const uint32_t rk[], int i) {
    return (uint8_t)(rk[i >> 2] >> (24 - 8 * (i & 3)));
}

void aes256_expand_key(const uint8_t* key, uint32_t rk[AES_RK_WORDS]) {
    for (int i = 0; i < 8; i++)
        rk[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
                ((uint32_t)key[i * 4 + 2] << 8) | key[i * 4 + 3];
    for (int i = 8; i < AES_RK_WORDS; i++) {
        uint32_t t = rk[i - 1];
        if (i % 8 == 0) {
            uint32_t rot = (t << 8) | (t >> 24);
            uint8_t b0 = AES.sbox[(rot >> 24) & 0xFF];
            uint8_t b1 = AES.sbox[(rot >> 16) & 0xFF];
            uint8_t b2 = AES.sbox[(rot >> 8) & 0xFF];
            uint8_t b3 = AES.sbox[rot & 0xFF];
            t = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                ((uint32_t)b2 << 8) | b3;
            t ^= (uint32_t)RCON[i / 8] << 24;
        } else if (i % 8 == 4) {
            uint8_t b0 = AES.sbox[(t >> 24) & 0xFF];
            uint8_t b1 = AES.sbox[(t >> 16) & 0xFF];
            uint8_t b2 = AES.sbox[(t >> 8) & 0xFF];
            uint8_t b3 = AES.sbox[t & 0xFF];
            t = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                ((uint32_t)b2 << 8) | b3;
        }
        rk[i] = rk[i - 8] ^ t;
    }
}

// ---- 正向变换（字节序 = 标准 AES 状态矩阵列主序） ----
void aes256_encrypt_block(const uint8_t* in, uint8_t* out,
                          const uint32_t rk[AES_RK_WORDS]) {
    uint8_t st[AES_BLOCK];
    for (int i = 0; i < AES_BLOCK; i++) st[i] = in[i] ^ rk_byte(rk, i);
    uint8_t t[AES_BLOCK];
    for (int round = 1; round <= AES_ROUNDS - 1; round++) {
        for (int i = 0; i < AES_BLOCK; i++) st[i] = AES.sbox[st[i]];  // SubBytes
        // ShiftRows
        t[0] = st[0];  t[1] = st[5];  t[2] = st[10];  t[3] = st[15];
        t[4] = st[4];  t[5] = st[9];  t[6] = st[14];  t[7] = st[3];
        t[8] = st[8];  t[9] = st[13]; t[10] = st[2];  t[11] = st[7];
        t[12] = st[12]; t[13] = st[1]; t[14] = st[6]; t[15] = st[11];
        // MixColumns（字节布局 state[r][c]=buf[4c+r]，每列 4 个连续字节）
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
            st[4 * c]     = AES.mul2[a0] ^ AES.mul3[a1] ^ a2 ^ a3;
            st[4 * c + 1] = a0 ^ AES.mul2[a1] ^ AES.mul3[a2] ^ a3;
            st[4 * c + 2] = a0 ^ a1 ^ AES.mul2[a2] ^ AES.mul3[a3];
            st[4 * c + 3] = AES.mul3[a0] ^ a1 ^ a2 ^ AES.mul2[a3];
        }
        for (int i = 0; i < AES_BLOCK; i++) st[i] ^= rk_byte(rk, 16 * round + i);
    }
    // 最后一轮（无 MixColumns）
    for (int i = 0; i < AES_BLOCK; i++) st[i] = AES.sbox[st[i]];
    t[0] = st[0];  t[1] = st[5];  t[2] = st[10];  t[3] = st[15];
    t[4] = st[4];  t[5] = st[9];  t[6] = st[14];  t[7] = st[3];
    t[8] = st[8];  t[9] = st[13]; t[10] = st[2];  t[11] = st[7];
    t[12] = st[12]; t[13] = st[1]; t[14] = st[6]; t[15] = st[11];
    for (int i = 0; i < AES_BLOCK; i++) out[i] = t[i] ^ rk_byte(rk, 16 * AES_ROUNDS + i);
}

// ---- 逆变换 ----
void aes256_decrypt_block(const uint8_t* in, uint8_t* out,
                          const uint32_t rk[AES_RK_WORDS]) {
    uint8_t st[AES_BLOCK];
    for (int i = 0; i < AES_BLOCK; i++) st[i] = in[i] ^ rk_byte(rk, 16 * AES_ROUNDS + i);
    uint8_t t[AES_BLOCK];
    for (int round = AES_ROUNDS - 1; round >= 1; round--) {
        // InvShiftRows
        t[0] = st[0];  t[4] = st[4];  t[8] = st[8];  t[12] = st[12];
        t[1] = st[13]; t[5] = st[1];  t[9] = st[5];  t[13] = st[9];
        t[2] = st[10]; t[6] = st[14]; t[10] = st[2]; t[14] = st[6];
        t[3] = st[7];  t[7] = st[11]; t[11] = st[15]; t[15] = st[3];
        // InvSubBytes
        for (int i = 0; i < AES_BLOCK; i++) t[i] = AES.inv_sbox[t[i]];
        // AddRoundKey
        for (int i = 0; i < AES_BLOCK; i++) t[i] ^= rk_byte(rk, 16 * round + i);
        // InvMixColumns（与 MixColumns 相同的列布局：每列 4 个连续字节）
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
            st[4 * c]     = AES.mul14[a0] ^ AES.mul11[a1] ^ AES.mul13[a2] ^ AES.mul9[a3];
            st[4 * c + 1] = AES.mul9[a0] ^ AES.mul14[a1] ^ AES.mul11[a2] ^ AES.mul13[a3];
            st[4 * c + 2] = AES.mul13[a0] ^ AES.mul9[a1] ^ AES.mul14[a2] ^ AES.mul11[a3];
            st[4 * c + 3] = AES.mul11[a0] ^ AES.mul13[a1] ^ AES.mul9[a2] ^ AES.mul14[a3];
        }
    }
    // 最后一轮
    t[0] = st[0];  t[4] = st[4];  t[8] = st[8];  t[12] = st[12];
    t[1] = st[13]; t[5] = st[1];  t[9] = st[5];  t[13] = st[9];
    t[2] = st[10]; t[6] = st[14]; t[10] = st[2]; t[14] = st[6];
    t[3] = st[7];  t[7] = st[11]; t[11] = st[15]; t[15] = st[3];
    for (int i = 0; i < AES_BLOCK; i++) t[i] = AES.inv_sbox[t[i]];
    for (int i = 0; i < AES_BLOCK; i++) out[i] = t[i] ^ rk_byte(rk, i);
}

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;

    Sha256Ctx() {
        h[0] = 0x6a09e667u; h[1] = 0xbb67ae85u; h[2] = 0x3c6ef372u; h[3] = 0xa54ff53au;
        h[4] = 0x510e527fu; h[5] = 0x9b05688cu; h[6] = 0x1f83d9abu; h[7] = 0x5be0cd19u;
        total = 0; buflen = 0;
    }

    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            size_t take = 64 - buflen;
            if (take > len) take = len;
            memcpy(buf + buflen, data, take);
            buflen += take;
            data += take;
            len -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    void final(uint8_t out[SHA256_DIGEST_LEN]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
        update(lenb, 8);
        for (int i = 0; i < 8; i++) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
    }
};

} // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    Sha256Ctx ctx;
    ctx.update(data, len);
    ctx.final(out);
}

void sha256_hex(const uint8_t* data, size_t len, char out_hex[65]) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(data, len, d);
    static const char* hexdig = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        out_hex[i * 2] = hexdig[d[i] >> 4];
        out_hex[i * 2 + 1] = hexdig[d[i] & 0xF];
    }
    out_hex[64] = 0;
}

// HMAC-SHA256，支持两段消息拼接（避免 AEAD 再拷贝 nonce||ciphertext）
static void hmac_sha256_parts(const uint8_t* key, size_t key_len,
                              const uint8_t* a, size_t a_len,
                              const uint8_t* b, size_t b_len,
                              uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        sha256(key, key_len, k);
    } else if (key && key_len) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256Ctx inner;
    inner.update(ipad, 64);
    if (a && a_len) inner.update(a, a_len);
    if (b && b_len) inner.update(b, b_len);
    uint8_t ih[SHA256_DIGEST_LEN];
    inner.final(ih);

    Sha256Ctx outer;
    outer.update(opad, 64);
    outer.update(ih, SHA256_DIGEST_LEN);
    outer.final(out);
}

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    hmac_sha256_parts(key, key_len, msg, msg_len, nullptr, 0, out);
}

bool p2p_const_time_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    if (n == 0) return true;
    if (!a || !b) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

void p2p_stream_xor(const uint8_t* secret, size_t secret_len,
                    const char* uuid, const uint8_t iv[8],
                    uint8_t* data, size_t len) {
    // PRF 输入：uuid(33B 补零) || iv(8) || block_idx(1)
    uint8_t prf[33 + 8 + 1];
    memset(prf, 0, sizeof(prf));
    memcpy(prf, uuid, 33);
    memcpy(prf + 33, iv, 8);

    size_t off = 0;
    while (off < len) {
        prf[33 + 8] = (uint8_t)(off / SHA256_DIGEST_LEN);
        uint8_t key[SHA256_DIGEST_LEN];
        hmac_sha256(secret, secret_len, prf, sizeof(prf), key);
        size_t n = len - off;
        if (n > SHA256_DIGEST_LEN) n = SHA256_DIGEST_LEN;
        for (size_t i = 0; i < n; i++) data[off + i] ^= key[i];
        off += n;
    }
}

int pbkdf2_hmac_sha256(const uint8_t* pw, size_t pw_len,
                       const uint8_t* salt, size_t salt_len,
                       uint32_t iterations,
                       uint8_t* out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    if (salt_len > 60 || (salt_len > 0 && !salt)) {
        memset(out, 0, out_len);
        return -1;
    }
    if (iterations == 0) iterations = 1;
    uint8_t buf[64];
    memcpy(buf, salt, salt_len);

    size_t off = 0;
    uint32_t block = 1;
    uint8_t u[SHA256_DIGEST_LEN];
    while (off < out_len) {
        buf[salt_len] = (uint8_t)(block >> 24);
        buf[salt_len + 1] = (uint8_t)(block >> 16);
        buf[salt_len + 2] = (uint8_t)(block >> 8);
        buf[salt_len + 3] = (uint8_t)block;
        hmac_sha256(pw, pw_len, buf, salt_len + 4, u);
        size_t n = out_len - off;
        if (n > SHA256_DIGEST_LEN) n = SHA256_DIGEST_LEN;
        memcpy(out + off, u, n);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_sha256(pw, pw_len, u, SHA256_DIGEST_LEN, u);
            for (size_t j = 0; j < n; j++) out[off + j] ^= u[j];
        }
        off += n;
        block++;
    }
    return 0;
}

int aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len) {
    uint32_t rk[AES_RK_WORDS];
    aes256_expand_key(key, rk);

    size_t pad = AES_BLOCK - (in_len % AES_BLOCK);   // PKCS7：1..16
    size_t total = in_len + pad;
    if (total > out_cap) return -1;

    uint8_t prev[AES_BLOCK];
    memcpy(prev, iv, AES_BLOCK);
    for (size_t blk = 0; blk < total; blk += AES_BLOCK) {
        uint8_t plain[AES_BLOCK];
        for (int i = 0; i < AES_BLOCK; i++) {
            size_t idx = blk + i;
            uint8_t b = idx < in_len ? in[idx] : (uint8_t)pad;
            plain[i] = b ^ prev[i];
        }
        uint8_t cipher[AES_BLOCK];
        aes256_encrypt_block(plain, cipher, rk);
        memcpy(out + blk, cipher, AES_BLOCK);
        memcpy(prev, cipher, AES_BLOCK);
    }
    if (out_len) *out_len = total;
    return 0;
}

int aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len) {
    if (in_len == 0 || in_len % AES_BLOCK != 0 || in_len > out_cap) return -1;
    uint32_t rk[AES_RK_WORDS];
    aes256_expand_key(key, rk);

    uint8_t prev[AES_BLOCK];
    memcpy(prev, iv, AES_BLOCK);
    for (size_t blk = 0; blk < in_len; blk += AES_BLOCK) {
        uint8_t cipher[AES_BLOCK];
        memcpy(cipher, in + blk, AES_BLOCK);
        uint8_t plain[AES_BLOCK];
        aes256_decrypt_block(cipher, plain, rk);
        for (int i = 0; i < AES_BLOCK; i++) {
            out[blk + i] = plain[i] ^ prev[i];
            prev[i] = cipher[i];
        }
    }
    // 校验并去除 PKCS7 填充
    size_t pad = out[in_len - 1];
    if (pad == 0 || pad > AES_BLOCK) return -1;
    for (size_t i = in_len - pad; i < in_len; i++)
        if (out[i] != pad) return -1;
    if (out_len) *out_len = in_len - pad;
    return 0;
}

// ---------------------------------------------------------------------------
// AEAD：AES-256-CTR + HMAC-SHA256（Encrypt-then-MAC）
// ---------------------------------------------------------------------------

// AES-256-CTR 模式加解密（对称，加密/解密同一函数）
static void aes256_ctr_xor(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* in, size_t len, uint8_t* out) {
    uint32_t rk[AES_RK_WORDS];
    aes256_expand_key(key, rk);

    // CTR 计数器：nonce(12B) || counter(4B, big-endian)
    uint8_t ctr[AES_BLOCK];
    memcpy(ctr, nonce, 12);
    memset(ctr + 12, 0, 4);

    size_t off = 0;
    uint32_t counter = 0;
    while (off < len) {
        // 生成密钥流块
        uint8_t keystream[AES_BLOCK];
        aes256_encrypt_block(ctr, keystream, rk);

        size_t n = len - off;
        if (n > AES_BLOCK) n = AES_BLOCK;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ keystream[i];
        off += n;

        // 递增计数器（big-endian）
        counter++;
        ctr[12] = (uint8_t)(counter >> 24);
        ctr[13] = (uint8_t)(counter >> 16);
        ctr[14] = (uint8_t)(counter >> 8);
        ctr[15] = (uint8_t)counter;
    }
}

int p2p_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap, size_t* out_len,
                     uint8_t tag[16]) {
    if (in_len > out_cap) return -1;

    // AES-256-CTR 加密
    aes256_ctr_xor(key, nonce, in, in_len, out);
    *out_len = in_len;

    // HMAC-SHA256 计算认证标签（Encrypt-then-MAC，分段更新避免大栈缓冲）
    uint8_t mac_key[SHA256_DIGEST_LEN];
    hmac_sha256(key, 32, (const uint8_t*)"aead-mac", 8, mac_key);

    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256_parts(mac_key, SHA256_DIGEST_LEN, nonce, 12, out, in_len, mac);
    memcpy(tag, mac, 16);

    return 0;
}

int p2p_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap, size_t* out_len,
                     const uint8_t tag[16]) {
    if (in_len > out_cap) return -1;

    uint8_t mac_key[SHA256_DIGEST_LEN];
    hmac_sha256(key, 32, (const uint8_t*)"aead-mac", 8, mac_key);

    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256_parts(mac_key, SHA256_DIGEST_LEN, nonce, 12, in, in_len, mac);

    if (!p2p_const_time_eq(mac, tag, 16)) return -1;

    // AES-256-CTR 解密
    aes256_ctr_xor(key, nonce, in, in_len, out);
    *out_len = in_len;

    return 0;
}

// ---------------------------------------------------------------------------
// Encryptor 抽象层实现（trait 抽象，支持运行时切换算法）
// ---------------------------------------------------------------------------

class XorEncryptor : public Encryptor {
    uint8_t key_[32];
    std::string uuid_;
    uint8_t iv_[8];
public:
    XorEncryptor(const uint8_t key[32], const char* uuid, const uint8_t iv[8]) {
        memcpy(key_, key, 32);
        if (uuid) uuid_ = uuid;
        if (iv) memcpy(iv_, iv, 8);
    }
    int encrypt(const uint8_t* in, size_t in_len,
                uint8_t* out, size_t out_cap, size_t* out_len) override {
        if (in_len > out_cap) return -1;
        memcpy(out, in, in_len);
        p2p_stream_xor(key_, 32, uuid_.c_str(), iv_, out, in_len);
        *out_len = in_len;
        return 0;
    }
    int decrypt(const uint8_t* in, size_t in_len,
                uint8_t* out, size_t out_cap, size_t* out_len) override {
        if (in_len > out_cap) return -1;
        memcpy(out, in, in_len);
        p2p_stream_xor(key_, 32, uuid_.c_str(), iv_, out, in_len);
        *out_len = in_len;
        return 0;
    }
    EncryptionAlgorithm algorithm() const override { return EncryptionAlgorithm::Xor; }
};

class AesCtrEncryptor : public Encryptor {
    uint8_t key_[32];
public:
    explicit AesCtrEncryptor(const uint8_t key[32]) { memcpy(key_, key, 32); }
    int encrypt(const uint8_t* in, size_t in_len,
                uint8_t* out, size_t out_cap, size_t* out_len) override {
        if (in_len > out_cap - 28) return -1;  // nonce(12) + tag(16)
        uint8_t nonce[12];
        if (p2p_random_bytes(nonce, 12) != 0) return -1;
        uint8_t tag[16];
        if (p2p_aead_encrypt(key_, nonce, in, in_len, out + 12, out_cap - 12, out_len, tag) != 0)
            return -1;
        memcpy(out, nonce, 12);
        memcpy(out + 12 + *out_len, tag, 16);
        *out_len += 28;
        return 0;
    }
    int decrypt(const uint8_t* in, size_t in_len,
                uint8_t* out, size_t out_cap, size_t* out_len) override {
        if (in_len <= 28) return -1;
        const uint8_t* nonce = in;
        size_t cipher_len = in_len - 28;
        const uint8_t* tag = in + 12 + cipher_len;
        return p2p_aead_decrypt(key_, nonce, in + 12, cipher_len, out, out_cap, out_len, tag);
    }
    EncryptionAlgorithm algorithm() const override { return EncryptionAlgorithm::Aes256Ctr; }
};

std::unique_ptr<Encryptor> create_encryptor(EncryptionAlgorithm alg,
                                            const uint8_t key[32],
                                            const char* uuid, const uint8_t iv[8]) {
    switch (alg) {
        case EncryptionAlgorithm::Xor:
            return std::make_unique<XorEncryptor>(key, uuid, iv);
        case EncryptionAlgorithm::Aes256Ctr:
            return std::make_unique<AesCtrEncryptor>(key);
        default:
            return nullptr;
    }
}

int p2p_random_bytes(uint8_t* out, size_t len) {
    if (len == 0) return 0;
    if (!out) return -1;

    size_t got = 0;
    while (got < len) {
        ssize_t n = getrandom(out + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)n;
    }
    if (got == len) return 0;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        got = 0;
        while (got < len) {
            ssize_t n = read(fd, out + got, len - got);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            got += (size_t)n;
        }
        close(fd);
        if (got == len) return 0;
    }

    memset(out, 0, len);
    return -1;
}

} // namespace p2p
