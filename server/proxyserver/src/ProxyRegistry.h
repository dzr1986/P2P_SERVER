#ifndef P2P_PROXY_REGISTRY_H
#define P2P_PROXY_REGISTRY_H

#include <chrono>
#include <string>
#include <unordered_map>

#include "ProtoDef.h"
#include "Util.h"

namespace p2p {

// 调用方持锁；UUID 与地址保持一一对应，租约只由注册请求续期。
class ProxyRegistry {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto lease = std::chrono::seconds(120);

    bool register_peer(const std::string& uuid, const sockaddr_in& addr,
                       size_t capacity, TimePoint now) {
        if (uuid.empty() || uuid.size() > MAX_UUID_LEN || capacity == 0) return false;
        const auto key = addr_to_u64(addr);
        if (peers_.size() >= capacity && peers_.find(uuid) == peers_.end() &&
            addresses_.find(key) == addresses_.end()) {
            expire(now);
            if (peers_.size() >= capacity) return false;
        }

        auto old = peers_.find(uuid);
        if (old != peers_.end()) addresses_.erase(addr_to_u64(old->second.addr));
        auto owner = addresses_.find(key);
        if (owner != addresses_.end()) {
            peers_.erase(owner->second);
            addresses_.erase(owner);
        }
        peers_[uuid] = {addr, now + lease};
        addresses_[key] = uuid;
        return true;
    }

    bool unregister_peer(const std::string& uuid, const sockaddr_in& addr) {
        auto it = peers_.find(uuid);
        if (it == peers_.end() || !sockaddr_eq(it->second.addr, addr)) return false;
        addresses_.erase(addr_to_u64(it->second.addr));
        peers_.erase(it);
        return true;
    }

    bool resolve(const std::string& source, const sockaddr_in& from,
                 const std::string& target, sockaddr_in& to, TimePoint now) const {
        auto src = peers_.find(source);
        if (src == peers_.end() || src->second.expires <= now ||
            !sockaddr_eq(src->second.addr, from)) return false;
        auto dst = peers_.find(target);
        if (dst == peers_.end() || dst->second.expires <= now) return false;
        to = dst->second.addr;
        return true;
    }

    bool registered(const std::string& uuid, const sockaddr_in& addr, TimePoint now) const {
        auto it = peers_.find(uuid);
        return it != peers_.end() && it->second.expires > now &&
               sockaddr_eq(it->second.addr, addr);
    }

    void expire(TimePoint now) {
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (it->second.expires <= now) {
                addresses_.erase(addr_to_u64(it->second.addr));
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t size() const { return peers_.size(); }

private:
    struct Entry {
        sockaddr_in addr;
        TimePoint expires;
    };
    std::unordered_map<std::string, Entry> peers_;
    std::unordered_map<uint64_t, std::string> addresses_;
};

} // namespace p2p

#endif // P2P_PROXY_REGISTRY_H
