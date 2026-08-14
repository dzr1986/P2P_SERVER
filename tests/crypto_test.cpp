// crypto_test.cpp：加密原语单测（NIST SP 800-38A AES-256-CBC 向量 + 往返/错误密钥 + PBKDF2 冒烟）
#include "common/Crypto.h"

#include <cstdio>
#include <cstring>

using namespace p2p;

static void parse_hex(const char* h, uint8_t* out) {
    for (int i = 0; h[i]; i += 2) {
        uint8_t hi = (uint8_t)(h[i] <= '9' ? h[i] - '0' : h[i] - 'a' + 10);
        uint8_t lo = (uint8_t)(h[i + 1] <= '9' ? h[i + 1] - '0' : h[i + 1] - 'a' + 10);
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
}

int main() {
    uint8_t key[32], iv[16];
    parse_hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key);
    parse_hex("000102030405060708090a0b0c0d0e0f", iv);

    const uint8_t pt[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};
    const uint8_t expect[64] = {
        0xf5,0x8c,0x4c,0x04,0xd6,0xe5,0xf1,0xba,0x77,0x9e,0xab,0xfb,0x5f,0x7b,0xfb,0xd6,
        0x9c,0xfc,0x4e,0x96,0x7e,0xdb,0x80,0x8d,0x67,0x9f,0x77,0x7b,0xc6,0x70,0x2c,0x7d,
        0x39,0xf2,0x33,0x69,0xa9,0xd9,0xba,0xcf,0xa5,0x30,0xe2,0x63,0x04,0x23,0x14,0x61,
        0xb2,0xeb,0x05,0xe2,0xc3,0x9b,0xe9,0xfc,0xda,0x6c,0x19,0x07,0x8c,0x6a,0x9d,0x1b};

    uint8_t ct[80], back[80];
    size_t ctlen = 0, ptlen = 0;
    if (aes256_cbc_encrypt(key, iv, pt, 64, ct, sizeof(ct), &ctlen) != 0) {
        printf("FAIL encrypt error\n");
        return 1;
    }
    if (memcmp(ct, expect, 64) != 0) {
        printf("FAIL NIST AES-256-CBC ciphertext mismatch\n");
        printf("got:  ");
        for (int i = 0; i < 16; i++) printf("%02x", ct[i]);
        printf("\n");
        printf("want: ");
        for (int i = 0; i < 16; i++) printf("%02x", expect[i]);
        printf("\n");
        return 1;
    }
    if (aes256_cbc_decrypt(key, iv, ct, ctlen, back, sizeof(back), &ptlen) != 0 ||
        ptlen != 64 || memcmp(back, pt, 64) != 0) {
        printf("FAIL AES roundtrip\n");
        return 1;
    }

    uint8_t badkey[32];
    memcpy(badkey, key, 32);
    badkey[0] ^= 0x01;
    if (aes256_cbc_decrypt(badkey, iv, ct, ctlen, back, sizeof(back), &ptlen) == 0) {
        printf("FAIL wrong-key decrypt should fail\n");
        return 1;
    }

    // 非 16 倍长负载（PKCS7 补块往返）
    const uint8_t odd[37] = {0xAA, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    uint8_t ct2[64], back2[64];
    size_t ct2len = 0, pt2len = 0;
    if (aes256_cbc_encrypt(key, iv, odd, sizeof(odd), ct2, sizeof(ct2), &ct2len) != 0 ||
        aes256_cbc_decrypt(key, iv, ct2, ct2len, back2, sizeof(back2), &pt2len) != 0 ||
        pt2len != sizeof(odd) || memcmp(back2, odd, sizeof(odd)) != 0) {
        printf("FAIL PKCS7 odd-length roundtrip\n");
        return 1;
    }

    // PBKDF2 确定性 + 长度任意
    uint8_t k1[32], k2[48];
    const uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    pbkdf2_hmac_sha256((const uint8_t*)"s3cr3t", 6, salt, 16, 1000, k1, 32);
    pbkdf2_hmac_sha256((const uint8_t*)"s3cr3t", 6, salt, 16, 1000, k2, 48);
    if (memcmp(k1, k2, 32) != 0) {
        printf("FAIL PBKDF2 nondeterministic / length mismatch\n");
        return 1;
    }

    printf("crypto tests PASS (NIST AES-256-CBC, roundtrip, wrong-key, PKCS7, PBKDF2)\n");
    return 0;
}
