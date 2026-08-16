#ifndef P2P_SERVER_REGISTRY_SYNC_H
#define P2P_SERVER_REGISTRY_SYNC_H

// 多 NatServer 注册表同步（学 guide/network/host-public-server.md 共享节点集群）。
// 只同步 UUID→地址，不转发业务、不做 mesh 路由。

#include "server/config/CfgFile.h"
#include "server/listener/LocalListeners.h"
#include "server/peers/PeerManage.h"
#include "core/packet/ProtoDef.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace p2p {

class RegistrySync {
public:
    using SendFn = std::function<bool(const sockaddr_in&, uint8_t, const void*, size_t)>;

    void bind(SendFn send, PeerManager* peers);
    void set_cfg(std::shared_ptr<const CfgData> cfg) { cfg_ = std::move(cfg); }
    void setup(const std::string& wan_ip, uint16_t nat_port,
               const LocalListeners& listeners);

    bool enabled() const { return enabled_ && !addrs_.empty(); }

    void send_signed(const sockaddr_in& to, uint8_t msg_id,
                     const uint8_t* payload, size_t plen);
    bool verify(const uint8_t* payload, size_t plen, size_t body_len) const;
    void broadcast(uint8_t msg_id, const void* payload, size_t plen,
                   const sockaddr_in* except);

    void broadcast_peer(const UuidReq& req, const std::string& extinfo,
                        const sockaddr_in& pub);
    void handle_entry(const SyncPeerEntry& e, const std::string& extinfo,
                      const sockaddr_in& from);
    void handle_del(const std::string& uuid, const sockaddr_in& from);
    void handle_snapshot_req(const sockaddr_in& from);
    void request_snapshot();

private:
    void send_peer_entry(const sockaddr_in& to, const Peer& p, uint8_t hop);
    bool seen(const std::string& uuid, uint32_t hb_time, const sockaddr_in& from);

    SendFn send_;
    PeerManager* peers_ = nullptr;
    std::shared_ptr<const CfgData> cfg_;
    bool enabled_ = false;
    std::vector<sockaddr_in> addrs_;
    std::mutex mu_;
    std::map<std::string, time_t> seen_;
};

} // namespace p2p

#endif
