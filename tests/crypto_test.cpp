// crypto_test.cpp：加密原语单测（NIST SP 800-38A AES-256-CBC 向量 + 往返/错误密钥 + PBKDF2 冒烟）
#include "common/Crypto.h"
#include "common/Handshake.h"
#include "common/X25519.h"

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
    if (pbkdf2_hmac_sha256((const uint8_t*)"s3cr3t", 6, salt, 16, 1000, k1, 32) != 0 ||
        pbkdf2_hmac_sha256((const uint8_t*)"s3cr3t", 6, salt, 16, 1000, k2, 48) != 0) {
        printf("FAIL PBKDF2 returned error\n");
        return 1;
    }
    if (memcmp(k1, k2, 32) != 0) {
        printf("FAIL PBKDF2 nondeterministic / length mismatch\n");
        return 1;
    }

    uint8_t kbad[32];
    uint8_t long_salt[61] = {0};
    if (pbkdf2_hmac_sha256((const uint8_t*)"s3cr3t", 6, long_salt, 61, 1000, kbad, 32) == 0) {
        printf("FAIL PBKDF2 should reject salt_len > 60\n");
        return 1;
    }

    // 常量时间比较
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t c[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    if (!p2p_const_time_eq(a, b, 8) || p2p_const_time_eq(a, c, 8)) {
        printf("FAIL const-time compare\n");
        return 1;
    }

    // 安全随机：成功且不全零
    uint8_t rnd[32] = {0};
    if (p2p_random_bytes(rnd, sizeof(rnd)) != 0) {
        printf("FAIL p2p_random_bytes\n");
        return 1;
    }
    int nz = 0;
    for (size_t i = 0; i < sizeof(rnd); i++) if (rnd[i]) nz++;
    if (nz == 0) {
        printf("FAIL p2p_random_bytes all-zero\n");
        return 1;
    }

    // AEAD 往返 + 篡改检测（超过旧 4096 栈限制的长度也走分段 HMAC）
    uint8_t aead_key[32];
    memcpy(aead_key, key, 32);
    uint8_t nonce[12];
    if (p2p_random_bytes(nonce, sizeof(nonce)) != 0) {
        printf("FAIL AEAD nonce random\n");
        return 1;
    }
    const char* aead_pt = "p2p-aead-roundtrip-payload";
    size_t aead_ptlen = strlen(aead_pt);
    uint8_t aead_ct[64], aead_back[64], tag[16];
    size_t aead_ctlen = 0, aead_backlen = 0;
    if (p2p_aead_encrypt(aead_key, nonce, (const uint8_t*)aead_pt, aead_ptlen,
                         aead_ct, sizeof(aead_ct), &aead_ctlen, tag) != 0 ||
        p2p_aead_decrypt(aead_key, nonce, aead_ct, aead_ctlen,
                         aead_back, sizeof(aead_back), &aead_backlen, tag) != 0 ||
        aead_backlen != aead_ptlen || memcmp(aead_back, aead_pt, aead_ptlen) != 0) {
        printf("FAIL AEAD roundtrip\n");
        return 1;
    }
    tag[0] ^= 0x01;
    if (p2p_aead_decrypt(aead_key, nonce, aead_ct, aead_ctlen,
                         aead_back, sizeof(aead_back), &aead_backlen, tag) == 0) {
        printf("FAIL AEAD should reject bad tag\n");
        return 1;
    }

    auto enc = create_encryptor(EncryptionAlgorithm::Aes256Ctr, aead_key);
    if (!enc) {
        printf("FAIL create_encryptor\n");
        return 1;
    }
    uint8_t boxed[128], plain[64];
    size_t boxed_len = 0, plain_len = 0;
    if (enc->encrypt((const uint8_t*)aead_pt, aead_ptlen, boxed, sizeof(boxed), &boxed_len) != 0 ||
        enc->decrypt(boxed, boxed_len, plain, sizeof(plain), &plain_len) != 0 ||
        plain_len != aead_ptlen || memcmp(plain, aead_pt, aead_ptlen) != 0) {
        printf("FAIL Encryptor AEAD roundtrip\n");
        return 1;
    }

    // RFC 7748 X25519 测试向量
    uint8_t alice_sk[32], bob_sk[32], alice_pk[32], bob_pk[32], s1[32], s2[32];
    parse_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_sk);
    parse_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob_sk);
    uint8_t want_alice_pk[32], want_bob_pk[32], want_shared[32];
    parse_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want_alice_pk);
    parse_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want_bob_pk);
    parse_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want_shared);
    uint8_t nine[32] = {9};
    if (x25519(alice_pk, alice_sk, nine) != 0 || memcmp(alice_pk, want_alice_pk, 32) != 0) {
        printf("FAIL X25519 Alice public\n");
        return 1;
    }
    if (x25519(bob_pk, bob_sk, nine) != 0 || memcmp(bob_pk, want_bob_pk, 32) != 0) {
        printf("FAIL X25519 Bob public\n");
        return 1;
    }
    if (x25519(s1, alice_sk, bob_pk) != 0 || x25519(s2, bob_sk, alice_pk) != 0 ||
        memcmp(s1, s2, 32) != 0 || memcmp(s1, want_shared, 32) != 0) {
        printf("FAIL X25519 shared secret\n");
        return 1;
    }

    // 握手：两端派生相同会话密钥，且不可由 PSK 离线得到
    uint8_t na[16], nb[16];
    memset(na, 0x11, 16);
    memset(nb, 0x22, 16);
    uint8_t fs1[32], fs2[32], psk_key[32];
    hs_derive_session_key(s1, alice_pk, na, bob_pk, nb, "UIDA", "UIDB", fs1);
    hs_derive_session_key(s1, bob_pk, nb, alice_pk, na, "UIDB", "UIDA", fs2);
    if (memcmp(fs1, fs2, 32) != 0) {
        printf("FAIL handshake derive not symmetric\n");
        return 1;
    }
    hmac_sha256((const uint8_t*)"s3cr3t", 6, (const uint8_t*)"P2P-TUNNEL-KEY:UIDA:UIDB",
                24, psk_key);
    if (memcmp(fs1, psk_key, 32) == 0) {
        printf("FAIL FS key must not equal PSK-derived key\n");
        return 1;
    }
    uint8_t hsmsg[HS_LEN];
    if (hs_write(hsmsg, sizeof(hsmsg), alice_pk, na, (const uint8_t*)"s3cr3t", 6) != HS_LEN) {
        printf("FAIL hs_write\n");
        return 1;
    }
    uint8_t rpub[32], rnonce[16];
    if (!hs_read(hsmsg, HS_LEN, rpub, rnonce, (const uint8_t*)"s3cr3t", 6) ||
        memcmp(rpub, alice_pk, 32) != 0) {
        printf("FAIL hs_read\n");
        return 1;
    }
    if (hs_read(hsmsg, HS_LEN, rpub, rnonce, (const uint8_t*)"wrongkey", 8)) {
        printf("FAIL hs_read should reject bad PSK mac\n");
        return 1;
    }

    printf("crypto tests PASS (NIST AES-256-CBC, roundtrip, wrong-key, PKCS7, PBKDF2, AEAD, RNG, X25519, FS-HS)\n");
    return 0;
}
