#ifndef P2P_SERVER_RELAY_HEALTH_H
#define P2P_SERVER_RELAY_HEALTH_H

// 专用中继健康表（学 guide：共享节点只帮建连，数据面走独立 Relay）。
// NatServer 不转发业务；这里只探测 / 择优 P2PProxy。

#include "server/config/CfgFile.h"
#include "core/packet/ProtoDef.h"

#include <arpa/inet.h>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace p2p {

struct ProxyHealth {
    std::string ip;
    uint16_t    port = 0;
    uint8_t     available = 0;
    uint16_t    used = 0;
    uint16_t    max_proxy = 0;
    time_t      last_seen = 0;
    uint8_t     down = 0;
};

class RelayHealth {
public:
    void collect(const sockaddr_in& from, uint16_t proxy_port, const ProxyAvailRsp& rsp);
    void mark_stale();
    void pick(ProxyCandidate out[3], uint8_t& count,
              const CfgData& cfg, uint16_t proxy_port, char prefer_region);
    uint32_t available_count() const;
    void visit(const std::function<void(const ProxyHealth&)>& fn) const;

private:
    mutable std::mutex mu_;
    std::vector<ProxyHealth> rows_;
};

} // namespace p2p

#endif
