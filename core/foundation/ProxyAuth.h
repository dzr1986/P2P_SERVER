#ifndef P2P_CORE_PROXY_AUTH_H
#define P2P_CORE_PROXY_AUTH_H

// 中继注册 HMAC：默认演示密钥；生产用 P2P_PROXY_AUTH_SECRET。
// 客户端与 p2p_proxy 必须同一把钥匙。

#include "core/foundation/Crypto.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace p2p {

constexpr const char* kProxyAuthDefault = "p2p-proxy-auth-2024";

inline std::string proxy_auth_secret() {
    if (const char* e = getenv("P2P_PROXY_AUTH_SECRET")) {
        if (e[0]) return std::string(e);
    }
    return kProxyAuthDefault;
}

inline void proxy_register_hmac(const std::string& uuid, uint8_t mac[32]) {
    const std::string key = proxy_auth_secret();
    hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                reinterpret_cast<const uint8_t*>(uuid.data()), uuid.size(), mac);
}

inline bool proxy_register_hmac_ok(const std::string& uuid, const uint8_t mac[32]) {
    if (!mac) return false;
    uint8_t expect[32];
    proxy_register_hmac(uuid, expect);
    return p2p_const_time_eq(expect, mac, 32);
}

}  // namespace p2p

#endif
