#ifndef P2P_COMMON_CONNECT_TOKEN_H
#define P2P_COMMON_CONNECT_TOKEN_H

// 连线 Token（计划书 P1）：业主用设备 AuthKey 签发，客户端 CONNECT 时出示。
// NatServer 用 master 派生 dst AuthKey 验签后才下发 CONNECT_ACK。
// TokenNonceCache：验签通过后记已用 nonce，过期前拒绝重放。

#include "core/packet/ProtoDef.h"
#include "core/foundation/Uid.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

constexpr size_t CONNECT_TOKEN_HEX_LEN = sizeof(ConnectToken) * 2;  // 104

// 签发：ttl_sec=0 表示立即过期（测试用）。成功返回 0。
int connect_token_issue(const uint8_t auth_key[AUTH_KEY_LEN],
                        const char* dst_uid, const char* src_uid,
                        uint32_t ttl_sec, ConnectToken& out);

// 验签：过期、MAC 错误、参数非法返回 false。
bool connect_token_verify(const uint8_t auth_key[AUTH_KEY_LEN],
                          const char* dst_uid, const char* src_uid,
                          const ConnectToken& tok, uint32_t now_unix);

std::string connect_token_to_hex(const ConnectToken& tok);
bool connect_token_from_hex(const std::string& hex, ConnectToken& out);

// 已用 nonce 表：claim 成功=首次使用；false=重放或表满且无法腾位。
// 条目在 expire 之后可被 purge 清掉。多收包线程安全。
class TokenNonceCache {
public:
    static constexpr size_t kMaxEntries = 4096;

    // expire 为 unix 秒（与 ConnectToken.expire 主机序一致）
    bool claim(const uint8_t nonce[16], uint32_t expire, uint32_t now) {
        if (!nonce) return false;
        const std::string k = key_of(nonce);
        std::lock_guard<std::mutex> lk(mu_);
        auto it = used_.find(k);
        if (it != used_.end()) {
            if (it->second >= now) return false;  // 未过期：重放
            used_.erase(it);                      // 过期条目让路（新签发几乎不会撞 nonce）
        }
        if (used_.size() >= kMaxEntries) purge_locked(now);
        if (used_.size() >= kMaxEntries) {
            used_.erase(used_.begin());           // 仍满：淘汰任意一条，保可用性
        }
        used_.emplace(k, expire);
        return true;
    }

    void purge(uint32_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        purge_locked(now);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return used_.size();
    }

private:
    static std::string key_of(const uint8_t nonce[16]) {
        static const char* hex = "0123456789abcdef";
        std::string s(32, '0');
        for (int i = 0; i < 16; i++) {
            s[i * 2] = hex[nonce[i] >> 4];
            s[i * 2 + 1] = hex[nonce[i] & 0xF];
        }
        return s;
    }

    void purge_locked(uint32_t now) {
        for (auto it = used_.begin(); it != used_.end();) {
            if (it->second < now) it = used_.erase(it);
            else ++it;
        }
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, uint32_t> used_;
};

} // namespace p2p

#endif // P2P_COMMON_CONNECT_TOKEN_H
