#include "ConnectToken.h"
#include "Crypto.h"

#include <arpa/inet.h>
#include <cstring>
#include <ctime>

namespace p2p {

namespace {

constexpr char kLabel[] = "P2P-CONN-TOKEN:";  // 16 字节（含冒号）
constexpr size_t kLabelLen = 16;
constexpr uint32_t kMaxTtl = 7 * 24 * 3600;   // 最长 7 天，防超大 expire

void build_mac_msg(uint8_t* msg, size_t* mlen,
                   const char* dst_uid, const char* src_uid,
                   uint32_t expire_be, const uint8_t nonce[16]) {
    // label(16) || dst(33) || src(33) || expire(4) || nonce(16) = 102
    memset(msg, 0, 102);
    memcpy(msg, kLabel, kLabelLen);
    memcpy(msg + kLabelLen, dst_uid, strnlen(dst_uid, MAX_UUID_LEN));
    memcpy(msg + kLabelLen + MAX_UUID_LEN + 1, src_uid, strnlen(src_uid, MAX_UUID_LEN));
    memcpy(msg + kLabelLen + 2 * (MAX_UUID_LEN + 1), &expire_be, 4);
    memcpy(msg + kLabelLen + 2 * (MAX_UUID_LEN + 1) + 4, nonce, 16);
    *mlen = 102;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

int connect_token_issue(const uint8_t auth_key[AUTH_KEY_LEN],
                        const char* dst_uid, const char* src_uid,
                        uint32_t ttl_sec, ConnectToken& out) {
    if (!auth_key || !dst_uid || !src_uid || !dst_uid[0] || !src_uid[0]) return -1;
    if (ttl_sec > kMaxTtl) return -1;
    memset(&out, 0, sizeof(out));
    const uint32_t now = (uint32_t)time(nullptr);
    const uint32_t expire = (ttl_sec == 0) ? (now > 10 ? now - 10 : 1) : now + ttl_sec;
    out.expire = htonl(expire);
    if (p2p_random_bytes(out.nonce, sizeof(out.nonce)) != 0) return -1;
    uint8_t msg[102];
    size_t mlen = 0;
    build_mac_msg(msg, &mlen, dst_uid, src_uid, out.expire, out.nonce);
    hmac_sha256(auth_key, AUTH_KEY_LEN, msg, mlen, out.mac);
    return 0;
}

bool connect_token_verify(const uint8_t auth_key[AUTH_KEY_LEN],
                          const char* dst_uid, const char* src_uid,
                          const ConnectToken& tok, uint32_t now_unix) {
    if (!auth_key || !dst_uid || !src_uid || !dst_uid[0] || !src_uid[0]) return false;
    const uint32_t expire = ntohl(tok.expire);
    if (expire < now_unix) return false;                 // 已过期
    if (expire > now_unix + kMaxTtl) return false;       // 过远的未来
    uint8_t msg[102];
    size_t mlen = 0;
    build_mac_msg(msg, &mlen, dst_uid, src_uid, tok.expire, tok.nonce);
    uint8_t expect[32];
    hmac_sha256(auth_key, AUTH_KEY_LEN, msg, mlen, expect);
    return p2p_const_time_eq(expect, tok.mac, 32);
}

std::string connect_token_to_hex(const ConnectToken& tok) {
    static const char* kHex = "0123456789abcdef";
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&tok);
    std::string s(sizeof(ConnectToken) * 2, '0');
    for (size_t i = 0; i < sizeof(ConnectToken); i++) {
        s[i * 2] = kHex[p[i] >> 4];
        s[i * 2 + 1] = kHex[p[i] & 0xF];
    }
    return s;
}

bool connect_token_from_hex(const std::string& hex, ConnectToken& out) {
    if (hex.size() != sizeof(ConnectToken) * 2) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(&out);
    for (size_t i = 0; i < sizeof(ConnectToken); i++) {
        const int hi = hex_nibble(hex[i * 2]);
        const int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        p[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

} // namespace p2p
