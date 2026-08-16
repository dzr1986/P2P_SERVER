#ifndef P2P_SERVER_LISTENER_LOCAL_LISTENERS_H
#define P2P_SERVER_LISTENER_LOCAL_LISTENERS_H

// 对齐 EasyTier LocalListenerUrls：登记本实例正在听的地址，
// 同步/拨号时跳过会 hairpin 回自己的目标。
// 不把 0.0.0.0 当成「匹配所有远端」（多机同端口会误伤）。

#include <arpa/inet.h>
#include <cstdint>
#include <vector>

namespace p2p {

struct ListenEndpoint {
    uint32_t addr_be = 0;  // 0 = bind-any
    uint16_t port = 0;
};

class LocalListeners {
public:
    void clear() { eps_.clear(); }

    void add(uint32_t addr_be, uint16_t port) {
        if (port == 0) return;
        for (const auto& e : eps_) {
            if (e.addr_be == addr_be && e.port == port) return;
        }
        eps_.push_back({addr_be, port});
    }

    void add_any(uint16_t port) { add(0, port); }

    void add_ip(const char* ip, uint16_t port) {
        if (!ip || !ip[0] || port == 0) return;
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) != 1) return;
        if (a.s_addr == 0) {
            add_any(port);
            return;
        }
        add(a.s_addr, port);
    }

    bool is_local(const sockaddr_in& a) const {
        const uint16_t p = ntohs(a.sin_port);
        const uint32_t ip = a.sin_addr.s_addr;
        for (const auto& e : eps_) {
            if (e.port != p) continue;
            if (e.addr_be != 0 && e.addr_be == ip) return true;
            if (e.addr_be == 0 && is_loopback_be(ip)) return true;
        }
        return false;
    }

    size_t size() const { return eps_.size(); }

    static bool is_loopback_be(uint32_t addr_be) {
        return (ntohl(addr_be) >> 24) == 127;
    }

private:
    std::vector<ListenEndpoint> eps_;
};

}  // namespace p2p

#endif
