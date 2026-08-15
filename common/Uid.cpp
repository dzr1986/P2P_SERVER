#include "Uid.h"
#include "Crypto.h"
#include "ProtoDef.h"

#include <cstring>

namespace p2p {

namespace {

// RFC 4648 Base32 字符集（大写，无易混淆填充）
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr int kAlphabetLen = 32;

bool is_b32(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
}

// CRC：SHA-256(前 17 字符) 取前 15bit -> 3 个 Base32 字符
void compute_crc(const char* head17, char out[UID_CRC_LEN]) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(reinterpret_cast<const uint8_t*>(head17), UID_LEN - UID_CRC_LEN, d);
    const uint16_t bits15 = (uint16_t)(((d[0] << 8) | d[1]) >> 1);   // 高 15bit
    out[0] = kAlphabet[(bits15 >> 10) & 0x1F];
    out[1] = kAlphabet[(bits15 >> 5) & 0x1F];
    out[2] = kAlphabet[bits15 & 0x1F];
}

} // namespace

int uid_generate(const char* prefix, char region, char out[UID_LEN + 1]) {
    if (!prefix || !out) return -1;
    if (strnlen(prefix, UID_PREFIX_LEN + 1) != UID_PREFIX_LEN) return -1;
    for (size_t i = 0; i < UID_PREFIX_LEN; i++)
        if (!is_b32(prefix[i])) return -1;
    if (!is_b32(region)) return -1;

    memcpy(out, prefix, UID_PREFIX_LEN);
    out[UID_PREFIX_LEN] = region;

    // RANDOM(12 字符)：每字符 5bit 安全随机
    constexpr size_t kRandLen = UID_LEN - UID_PREFIX_LEN - 1 - UID_CRC_LEN;   // 12
    uint8_t rnd[kRandLen];
    if (p2p_random_bytes(rnd, sizeof(rnd)) != 0) return -1;
    for (size_t i = 0; i < kRandLen; i++)
        out[UID_PREFIX_LEN + 1 + i] = kAlphabet[rnd[i] % kAlphabetLen];

    compute_crc(out, out + UID_LEN - UID_CRC_LEN);
    out[UID_LEN] = 0;
    return 0;
}

bool uid_valid(const char* uid) {
    if (!uid) return false;
    if (strnlen(uid, UID_LEN + 1) != UID_LEN) return false;
    for (size_t i = 0; i < UID_LEN; i++)
        if (!is_b32(uid[i])) return false;
    char crc[UID_CRC_LEN];
    compute_crc(uid, crc);
    return memcmp(crc, uid + UID_LEN - UID_CRC_LEN, UID_CRC_LEN) == 0;
}

void uid_derive_auth_key(const uint8_t* master, size_t master_len,
                         const char* uid, uint8_t out[AUTH_KEY_LEN]) {
    // 域分隔标签防止与其它 HMAC 用途（登录 MAC/隧道密钥）交叉
    uint8_t msg[16 + MAX_UUID_LEN + 1] = {0};
    static const char kLabel[] = "P2P-UID-AUTHKEY:";
    memcpy(msg, kLabel, 16);
    const size_t ulen = strnlen(uid, MAX_UUID_LEN);
    memcpy(msg + 16, uid, ulen);
    hmac_sha256(master, master_len, msg, 16 + ulen, out);
}

std::string auth_key_to_hex(const uint8_t key[AUTH_KEY_LEN]) {
    static const char* kHex = "0123456789abcdef";
    std::string s(AUTH_KEY_LEN * 2, '0');
    for (size_t i = 0; i < AUTH_KEY_LEN; i++) {
        s[i * 2] = kHex[key[i] >> 4];
        s[i * 2 + 1] = kHex[key[i] & 0xF];
    }
    return s;
}

bool auth_key_from_hex(const std::string& hex, uint8_t out[AUTH_KEY_LEN]) {
    if (hex.size() != AUTH_KEY_LEN * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < AUTH_KEY_LEN; i++) {
        const int hi = nib(hex[i * 2]);
        const int lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

} // namespace p2p
