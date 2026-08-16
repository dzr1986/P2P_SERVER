#ifndef P2P_SERVER_CONNECTIVITY_MAPPING_OBSERVE_H
#define P2P_SERVER_CONNECTIVITY_MAPPING_OBSERVE_H

// 对齐 EasyTier stun/collector：服务端记下「同一公网 IP 打主口 / 备口」的映射。
// CONNECT 时若客户端还没上报 nattype，用这里的观察值补 hint。
// 只按 IP 关联（EDM 换端口）；同 IP 并发探测可能互扰，窗口默认 90s。

#include "core/packet/ProtoDef.h"

#include <arpa/inet.h>
#include <ctime>
#include <map>
#include <mutex>

namespace p2p {

class MappingObserve {
public:
    explicit MappingObserve(int ttl_sec = 90) : ttl_sec_(ttl_sec) {}

    void note(const sockaddr_in& from, uint8_t sock_idx) {
        if (sock_idx > 1) return;
        const uint32_t ip = from.sin_addr.s_addr;
        const uint16_t port = ntohs(from.sin_port);
        const time_t now = time(nullptr);
        std::lock_guard<std::mutex> lk(mu_);
        Sample& s = by_ip_[ip];
        if (s.t && now - s.t > ttl_sec_) s = Sample{};
        s.t = now;
        if (sock_idx == 0) s.main_port = port;
        else s.alt_port = port;
        if (s.main_port && s.alt_port) {
            s.nat = (s.main_port == s.alt_port) ? (uint8_t)NAT_PORT_RESTRICTED
                                                : (uint8_t)NAT_SYMMETRIC;
        }
    }

    uint8_t inferred_nat(const sockaddr_in& from) const {
        const uint32_t ip = from.sin_addr.s_addr;
        const time_t now = time(nullptr);
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_ip_.find(ip);
        if (it == by_ip_.end()) return NAT_UNKNOWN;
        if (!it->second.t || now - it->second.t > ttl_sec_) return NAT_UNKNOWN;
        return it->second.nat;
    }

    void cleanup(time_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = by_ip_.begin(); it != by_ip_.end();) {
            if (!it->second.t || now - it->second.t > ttl_sec_)
                it = by_ip_.erase(it);
            else
                ++it;
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return by_ip_.size();
    }

private:
    struct Sample {
        uint16_t main_port = 0;
        uint16_t alt_port = 0;
        uint8_t  nat = NAT_UNKNOWN;
        time_t   t = 0;
    };

    int ttl_sec_;
    mutable std::mutex mu_;
    std::map<uint32_t, Sample> by_ip_;
};

}  // namespace p2p

#endif
