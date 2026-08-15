#ifndef P2P_NAT_SERVER_H
#define P2P_NAT_SERVER_H

#include "AntiAbuse.h"
#include "CfgFile.h"
#include "LicenseMgr.h"
#include "NatTypeCheck.h"
#include "PeerManage.h"
#include "ProtoDef.h"
#include "StatusServer.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace p2p {

// 接收队列元素
struct IncomingPacket {
    uint8_t     buf[MAX_PKT];
    size_t      len;
    sockaddr_in from;
};

struct ProxyHealth {
    std::string ip;
    uint16_t    port;
    uint8_t     available;
    uint16_t    used;
    uint16_t    max_proxy;
    time_t      last_seen;
    uint8_t     down;        // 连续探测未响应次数（>=2 视为不可用）
};

// ---------------------------------------------------------------------------
// NatServer：UDP 汇聚/打洞协调服务端
// 线程模型（对标原版 NatServer.cpp + RecvProcess.cpp）：
//   recv_thread    ：epoll 收主 socket，NAT 探测走快速路径，其余入有界队列
//   proc_pool xN    ：出队处理业务报文
//   nattype_thread  ：select 备用 socket（端口变更/重协商快速路径）
//   timer_thread    ：心跳超时清理 / 黑名单过期 / 代理可用性探测 / 统计 / 热加载
//   status_thread   ：（可选）TCP JSON 状态服务
// ---------------------------------------------------------------------------
class NatServer {
public:
    NatServer();
    ~NatServer();

    int  init(const std::string& cfg_path, uint16_t nat_port,
              uint16_t proxy_port, const std::string& wan_ip,
              uint16_t status_port);
    int  run();
    void request_stop() { running_ = false; }

    // 供 RecvProcess / 线程使用
    std::shared_ptr<const CfgData> cfg() const { return cfg_; }
    PeerManager&   peers() { return peers_; }
    AntiAbuse&     anti_abuse() { return abuse_; }
    LicenseMgr&    license() { return license_; }
    NatTypeCheck&  natcheck() { return natcheck_; }
    uint16_t       proxy_port() const { return proxy_port_; }
    uint16_t       nat_port() const { return nat_port_; }

    bool send_msg(const sockaddr_in& to, uint8_t msg_id, const void* payload, size_t plen);
    // 负载末尾附加 HMAC(secret, payload) 后发送（未配置密钥则原样发送）
    void sync_send_msg(const sockaddr_in& to, uint8_t msg_id,
                       const uint8_t* payload, size_t plen);
    void inc_connect_ok() { connect_ok_.fetch_add(1); }
    void inc_connect_fail() { connect_fail_.fetch_add(1); }

    // 鉴权挑战/登录（挑战应答）
    bool issue_auth_nonce(const std::string& uuid, uint8_t out_nonce[16]);
    bool verify_auth_login(const std::string& uuid, const uint8_t nonce[16],
                           const uint8_t mac[32]);

    // 供 RecvProcess 调用的心跳处理（明文/加密共用）
    void handle_heartbeat(const UuidReq& req, const std::string& extinfo,
                          const sockaddr_in& from, bool enc,
                          const uint8_t enc_iv[8]);

    // 供 RecvProcess 调用的注册表同步处理
    void handle_sync_entry(const SyncPeerEntry& e, const std::string& extinfo,
                           const sockaddr_in& from);
    void handle_sync_del(const std::string& uuid, const sockaddr_in& from);
    void handle_sync_snapshot_req(const sockaddr_in& from);
    // 校验同步消息尾部 HMAC（未配置 SyncAuthSecret 视为放行）
    //   plen=收到负载总长，body_len=被签名负载长度（MAC 位于 payload+body_len 起 32B）
    bool sync_verify(const uint8_t* payload, size_t plen, size_t body_len) const;

private:
    void recv_thread(int idx);
    void proc_pool_thread(int id);
    void timer_thread();

    // SO_REUSEPORT 克隆收包 socket（与主端口同端口）
    int  make_recv_socket(uint16_t port);

    // 报文分发（RecvProcess.cpp）：按 msg_id 派发到各消息处理器
    void handle_packet(const uint8_t* data, size_t len, const sockaddr_in& from);
    void on_msg_heartbeat_plain(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_heartbeat_enc(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_extinfo(uint8_t msg_id, const uint8_t* p, size_t plen,
                        const sockaddr_in& from);
    void on_msg_connect_req(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_ice_sdp(uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_dev_list(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_server_list(const sockaddr_in& from);
    void on_msg_delete_uid(const uint8_t* p, size_t plen);
    void on_msg_check_uid(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_auth_challenge(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_auth_login(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_admin_stats(const sockaddr_in& from);
    void on_msg_admin_blacklist(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_sync_entry(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_sync_del(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_sync_snapshot(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_msg_proxy_avail(const uint8_t* p, size_t plen, const sockaddr_in& from);

    void send_heartbeat_rsp(const sockaddr_in& to, const char* uuid,
                            const ExtInfoRsp& rsp, bool enc,
                            const uint8_t enc_iv[8]);
    void send_proxy_avail_query(const std::string& proxy_ip);
    void collect_proxy_avail(const sockaddr_in& from, const ProxyAvailRsp& rsp);
    void mark_proxy_stale();
    void pick_proxy(ProxyCandidate out[3], uint8_t& count);
    std::string status_json() const;

    // 注册表同步
    void setup_sync_peers();                          // init 后解析同步对端
    void broadcast_sync_msg(uint8_t msg_id, const void* payload, size_t plen,
                            const sockaddr_in* except);
    void broadcast_peer_entry(const UuidReq& req, const std::string& extinfo,
                              const sockaddr_in& pub);
    void send_peer_entry(const sockaddr_in& to, const Peer& p, uint8_t hop);
    void send_sync_snapshot(const sockaddr_in& to);
    void request_sync_snapshot();                     // 向所有同步对端请求全量
    bool sync_enabled() const { return sync_enabled_ && !sync_addrs_.empty(); }
    bool seen_sync_entry(const std::string& uuid, uint32_t hb_time,
                         const sockaddr_in& from);    // 防环去重，返回 true=已见

    // 配置（shared_ptr 原子替换，支持热加载）
    std::shared_ptr<CfgData> cfg_;
    uint64_t cfg_mtime_ = 0;

    uint16_t nat_port_ = 0;
    uint16_t proxy_port_ = 0;
    uint16_t status_port_ = 0;
    std::string wan_ip_ = "0.0.0.0";

    PeerManager  peers_;
    AntiAbuse    abuse_;
    LicenseMgr   license_;
    NatTypeCheck natcheck_;
    StatusServer status_;

    // 注册表同步状态
    bool sync_enabled_ = false;
    std::vector<sockaddr_in> sync_addrs_;       // 已解析的同步对端
    std::mutex  sync_mu_;
    std::map<std::string, time_t> sync_seen_;   // 已见条目：uuid|hb_time|src -> 时间

    std::mutex              mq_mu_;
    std::condition_variable mq_cv_;
    std::deque<IncomingPacket> mq_;

    // 多收包线程：idx 0 用主 socket，其余用 SO_REUSEPORT 克隆 socket
    std::vector<int>        recv_socks_;

    // 代理健康表（SP_ASK_EXTINFO_RSP 刷新）
    std::mutex proxy_mu_;
    std::vector<ProxyHealth> proxy_health_;

    // 鉴权 nonce 表（uuid -> {nonce, 过期}）
    std::mutex nonce_mu_;
    std::unordered_map<std::string, std::pair<std::array<uint8_t, 16>, time_t>> auth_nonces_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_pkts_{0};
    std::atomic<uint64_t> connect_ok_{0};
    std::atomic<uint64_t> connect_fail_{0};
};

} // namespace p2p

#endif // P2P_NAT_SERVER_H
