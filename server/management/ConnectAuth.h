#ifndef P2P_CONNECT_AUTH_H
#define P2P_CONNECT_AUTH_H

// CONNECT 尾部 Token 验签（从 NatServer 抽出，学 guide secure-mode）。
// 密码学原语仍在 core/foundation/ConnectToken.h。

#include "core/foundation/ConnectToken.h"
#include "core/foundation/Log.h"
#include "core/foundation/Uid.h"
#include "server/config/CfgFile.h"

#include <cstdint>
#include <cstring>
#include <ctime>

namespace p2p {

inline bool verify_connect_trailer(const CfgData& cfg, TokenNonceCache& nonces,
                                   const char* src_uuid, const char* dst_uuid,
                                   const uint8_t* trailer, size_t tlen) {
    if (!cfg.enable_connect_token) return true;
    if (cfg.auth_secret.empty()) return false;
    if (!src_uuid || !dst_uuid) return false;
    if (!trailer || tlen < sizeof(ConnectToken)) return false;
    ConnectToken tok{};
    memcpy(&tok, trailer, sizeof(tok));
    uint8_t key[AUTH_KEY_LEN];
    uid_derive_auth_key(reinterpret_cast<const uint8_t*>(cfg.auth_secret.data()),
                        cfg.auth_secret.size(), dst_uuid, key);
    const uint32_t now = (uint32_t)time(nullptr);
    if (!connect_token_verify(key, dst_uuid, src_uuid, tok, now)) return false;
    if (!nonces.claim(tok.nonce, ntohl(tok.expire), now)) {
        LOGW("ConnectAuth", "CONNECT token replay src[%s] dst[%s]", src_uuid, dst_uuid);
        return false;
    }
    return true;
}

}  // namespace p2p

#endif
