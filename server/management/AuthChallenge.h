#ifndef P2P_SERVER_AUTH_CHALLENGE_H
#define P2P_SERVER_AUTH_CHALLENGE_H

// 报到挑战应答（学 guide/network/secure-mode.md 的「分级凭据」思路，不抄 Noise/WG）。
// 每 UID 独立 AuthKey；PrivateMode 只是强制 EnableAuth。

#include "server/config/CfgFile.h"
#include "core/foundation/Crypto.h"
#include "core/foundation/Uid.h"
#include "core/packet/ProtoDef.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

class AuthChallenge {
public:
    bool issue(const std::string& uuid, uint8_t out_nonce[16]) {
        if (p2p_random_bytes(out_nonce, 16) != 0) return false;
        std::lock_guard<std::mutex> lk(mu_);
        std::array<uint8_t, 16> n;
        memcpy(n.data(), out_nonce, 16);
        nonces_[uuid] = {n, time(nullptr) + 30};
        return true;
    }

    // 未启用鉴权时放行。成功不改注册表（由调用方 set_auth_expire）。
    bool verify(const CfgData& cfg, const std::string& uuid,
                const uint8_t nonce[16], const uint8_t mac[32]) {
        if (!cfg.auth_enabled()) return true;
        std::array<uint8_t, 16> n{};
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = nonces_.find(uuid);
            if (it == nonces_.end()) return false;
            if (time(nullptr) > it->second.second) { nonces_.erase(it); return false; }
            n = it->second.first;
            nonces_.erase(it);
        }
        if (!p2p_const_time_eq(n.data(), nonce, 16)) return false;
        uint8_t uid_key[AUTH_KEY_LEN];
        uid_derive_auth_key((const uint8_t*)cfg.auth_secret.data(),
                            cfg.auth_secret.size(), uuid.c_str(), uid_key);
        uint8_t msg[16 + MAX_UUID_LEN + 1];
        memset(msg, 0, sizeof(msg));
        memcpy(msg, uuid.c_str(), uuid.size());
        memcpy(msg + MAX_UUID_LEN + 1, nonce, 16);
        uint8_t expect[32];
        hmac_sha256(uid_key, AUTH_KEY_LEN, msg, 16 + MAX_UUID_LEN + 1, expect);
        return p2p_const_time_eq(expect, mac, 32);
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::pair<std::array<uint8_t, 16>, time_t>> nonces_;
};

} // namespace p2p

#endif
