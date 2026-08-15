#ifndef P2P_PROXY_H
#define P2P_PROXY_H

#include "Net.h"
#include "ProtoDef.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// UDP 中继代理（TURN 类兜底，对标原版 proxyserver/src/P2PProxy.cpp）
//   - 代理注册：uuid->地址 + 源地址->目标地址 双向中转路径（计数）
//   - RELAY_DATA 查表转发（打洞失败兜底）
//   - PUNCH_HELPER 打洞协助：返回目标当前公网地址
//   - SP_ASK_EXTINFO 可用性查询（供 NatServer 调度）
// 线程模型：SO_REUSEPORT 多 socket 多收包线程 + 定时回收线程
// 锁模型：注册表 shared_mutex（读多写少）+ 转发表按源地址分片
// ---------------------------------------------------------------------------
class P2PProxy {
public:
    P2PProxy() = default;
    ~P2PProxy() = default;
    P2PProxy(const P2PProxy&) = delete;
    P2PProxy& operator=(const P2PProxy&) = delete;

    int  init(uint16_t port, uint16_t max_proxy, int workers);
    void run();                     // 阻塞：启动各线程后循环等待
    void request_stop() { running_ = false; }

private:
    static constexpr int    kPathShards = 16;
    static constexpr size_t kMaxPathsPerSrc = 256;

    struct PathInfo {
        sockaddr_in dst{};          // 目标地址
        uint32_t    count = 0;      // 连接计数（对照 CountRecoverInMapAddrAddr）
        std::string uuid;           // 目标 uuid（仅用于日志）
        time_t      last_active = 0;// 最近一次命中
    };
    struct UuidEntry {
        sockaddr_in addr{};
        time_t      last_reg = 0;   // 最近注册时间
    };
    struct PathShard {
        std::mutex mu;
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, PathInfo>> paths;
    };

    // 线程体
    void recv_loop(const UdpFd& sock);
    void timer_loop();

    // 报文分发与各消息处理器（handle 按 msg_id 派发）
    void handle(uint8_t* data, size_t len, const sockaddr_in& from);
    void on_register_req(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_unregister_req(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_relay_data(uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_punch_helper(uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_avail_query(const sockaddr_in& from);

    // 注册表与路径表操作
    void do_register(const std::string& uuid, const sockaddr_in& from,
                     bool with_rsp, const uint8_t* hmac = nullptr);
    void unregister(const std::string& uuid, const sockaddr_in& from);
    void touch_path(uint64_t src_key, uint64_t dst_key,
                    const sockaddr_in& dst, const std::string& uuid);
    void erase_paths_for(uint64_t addr_key);
    static int shard_of(uint64_t key) { return (int)(key & (kPathShards - 1)); }

    int  send(const sockaddr_in& to, uint8_t msg_id,
              const void* payload, size_t plen);
    size_t registered_count() const {
        std::shared_lock<std::shared_mutex> lk(reg_mu_);
        return uuid2addr_.size();
    }

    uint16_t port_ = 0;
    uint16_t max_proxy_ = 0;
    int      workers_ = 4;
    std::vector<UdpFd> socks_;      // RAII：析构自动关闭

    mutable std::shared_mutex reg_mu_;
    std::unordered_map<std::string, UuidEntry> uuid2addr_;      // uuid -> 地址
    std::unordered_map<uint64_t, std::string>  addr2uuid_;      // 地址 -> uuid
    PathShard path_shards_[kPathShards];

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> relay_pkts_{0};
    std::atomic<uint64_t> relay_bytes_{0};
};

} // namespace p2p

#endif // P2P_PROXY_H
