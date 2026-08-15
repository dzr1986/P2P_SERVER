#ifndef P2P_COMMON_HANDSHAKE_H
#define P2P_COMMON_HANDSHAKE_H

// P5 前向保密握手（头文件实现，便于单测）
//   通道：可靠 TT_DATA，channel=0xFE（IOTC 应用通道 0~31 不可见）
//   报文：magic(1)=0xE1 | ver(1)=1 | type(1)=HELLO | pub(32) | nonce(16) | mac(16)
//   会话密钥：HMAC-SHA256(X25519(shared), "P2P-FS-v1" || 排序(pub||nonce) || 排序 uid)
//   AuthKey/PSK 不进入会话密钥（前向保密：泄露长期密钥无法离线推导历史会话）

#include <cstdint>
#include <cstring>
#include <string>

#include "core/foundation/Crypto.h"
#include "core/foundation/X25519.h"

namespace p2p {

constexpr uint8_t  HS_MAGIC = 0xE1;
constexpr uint8_t  HS_VER = 1;
constexpr uint8_t  HS_HELLO = 1;
constexpr uint8_t  HS_ACK = 2;
constexpr uint8_t  HS_CHANNEL = 0xFE;
constexpr size_t   HS_PUB = 32;
constexpr size_t   HS_NONCE = 16;
constexpr size_t   HS_MAC = 16;
constexpr size_t   HS_LEN = 3 + HS_PUB + HS_NONCE + HS_MAC;
constexpr char     HS_LABEL[] = "P2P-FS-v1";

inline size_t hs_write(uint8_t* out, size_t cap, const uint8_t pub[32],
                       const uint8_t nonce[16], const uint8_t* psk, size_t psk_len,
                       uint8_t type = HS_HELLO) {
    if (!out || cap < HS_LEN || !pub || !nonce) return 0;
    if (type != HS_HELLO && type != HS_ACK) return 0;
    out[0] = HS_MAGIC;
    out[1] = HS_VER;
    out[2] = type;
    memcpy(out + 3, pub, HS_PUB);
    memcpy(out + 3 + HS_PUB, nonce, HS_NONCE);
    memset(out + 3 + HS_PUB + HS_NONCE, 0, HS_MAC);
    if (psk && psk_len > 0) {
        uint8_t mac[32];
        hmac_sha256(psk, psk_len, out, 3 + HS_PUB + HS_NONCE, mac);
        memcpy(out + 3 + HS_PUB + HS_NONCE, mac, HS_MAC);
    }
    return HS_LEN;
}

inline bool hs_read(const uint8_t* buf, size_t len, uint8_t pub[32],
                    uint8_t nonce[16], const uint8_t* psk, size_t psk_len) {
    if (!buf || len != HS_LEN || buf[0] != HS_MAGIC || buf[1] != HS_VER ||
        (buf[2] != HS_HELLO && buf[2] != HS_ACK) || !pub || !nonce) {
        return false;
    }
    if (psk && psk_len > 0) {
        uint8_t mac[32];
        hmac_sha256(psk, psk_len, buf, 3 + HS_PUB + HS_NONCE, mac);
        if (!p2p_const_time_eq(mac, buf + 3 + HS_PUB + HS_NONCE, HS_MAC)) return false;
    }
    memcpy(pub, buf + 3, HS_PUB);
    memcpy(nonce, buf + 3 + HS_PUB, HS_NONCE);
    return true;
}

inline void hs_derive_session_key(const uint8_t shared[32],
                                  const uint8_t pub_a[32], const uint8_t nonce_a[16],
                                  const uint8_t pub_b[32], const uint8_t nonce_b[16],
                                  const char* uid_a, const char* uid_b,
                                  uint8_t out[32]) {
    uint8_t t1[HS_PUB + HS_NONCE], t2[HS_PUB + HS_NONCE];
    memcpy(t1, pub_a, HS_PUB);
    memcpy(t1 + HS_PUB, nonce_a, HS_NONCE);
    memcpy(t2, pub_b, HS_PUB);
    memcpy(t2 + HS_PUB, nonce_b, HS_NONCE);
    if (memcmp(t1, t2, sizeof(t1)) > 0) {
        uint8_t tmp[sizeof(t1)];
        memcpy(tmp, t1, sizeof(t1));
        memcpy(t1, t2, sizeof(t1));
        memcpy(t2, tmp, sizeof(t1));
    }
    std::string u1 = uid_a ? uid_a : "";
    std::string u2 = uid_b ? uid_b : "";
    if (u1 > u2) std::swap(u1, u2);

    uint8_t msg[sizeof(HS_LABEL) + sizeof(t1) + sizeof(t2) + 66];
    size_t n = 0;
    memcpy(msg + n, HS_LABEL, sizeof(HS_LABEL) - 1);
    n += sizeof(HS_LABEL) - 1;
    memcpy(msg + n, t1, sizeof(t1));
    n += sizeof(t1);
    memcpy(msg + n, t2, sizeof(t2));
    n += sizeof(t2);
    memcpy(msg + n, u1.data(), u1.size());
    n += u1.size();
    msg[n++] = 0;
    memcpy(msg + n, u2.data(), u2.size());
    n += u2.size();
    hmac_sha256(shared, 32, msg, n, out);
}

} // namespace p2p

#endif // P2P_COMMON_HANDSHAKE_H
