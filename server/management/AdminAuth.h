#ifndef P2P_ADMIN_AUTH_H
#define P2P_ADMIN_AUTH_H

// UDP 管理统计：配了 AdminSecret 时要求 32B HMAC(secret, msg_id)。
// 未配置密钥保持放行（本地/CI）；与黑名单「无密钥则拒绝写操作」不同。

#include "core/foundation/Crypto.h"
#include "core/packet/ProtoDef.h"
#include "server/config/CfgFile.h"

#include <cstdint>

namespace p2p {

inline bool verify_admin_stats(const CfgData& cfg, const uint8_t* trailer, size_t tlen) {
    if (cfg.admin_secret.empty()) return true;
    if (!trailer || tlen < 32) return false;
    uint8_t expect[32];
    const uint8_t id = MSG_ADMIN_STATS_REQ;
    hmac_sha256(reinterpret_cast<const uint8_t*>(cfg.admin_secret.data()),
                cfg.admin_secret.size(), &id, 1, expect);
    return p2p_const_time_eq(expect, trailer, 32);
}

}  // namespace p2p

#endif
