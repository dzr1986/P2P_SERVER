#ifndef P2P_PROXY_H
#define P2P_PROXY_H

#include "ProtoDef.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <mutex>
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
// ---------------------------------------------------------------------------
class P2PProxy {
public:
    P2PProxy();
    ~P2PProxy();

    int  init(uint16_t port, uint16_t max_proxy, int workers);
    void run();                     // 阻塞：启动各线程后循环等待
    void request_stop() { running_ = false; }

private:
    struct PathInfo {
        sockaddr_in dst;            // 目标地址
        uint32_t    count;          // 连接计数（对照 CountRecoverInMapAddrAddr）
        std::string uuid;           // 目标 uuid（仅用于日志）
        time_t      last_active;    // 最近一次命中
    };
    struct UuidEntry {
        sockaddr_in addr;
        time_t      last_reg;       // 最近注册时间
    };

    void recv_loop(int fd);
    void timer_loop();
    void handle(uint8_t* data, size_t len, const sockaddr_in& from);
    void do_register(const std::string& uuid, const sockaddr_in& from,
                     bool with_rsp, const uint8_t* hmac = nullptr);
    void unregister(const std::string& uuid, const sockaddr_in& from);
    int  send(const sockaddr_in& to, uint8_t msg_id,
              const void* payload, size_t plen);
    int  out_fd() const { return fds_.empty() ? -1 : fds_[0]; }

    uint16_t port_ = 0;
    uint16_t max_proxy_ = 0;
    int      workers_ = 4;
    std::vector<int> fds_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, UuidEntry> uuid2addr_;      // uuid -> 地址
    std::unordered_map<uint64_t, std::string>  addr2uuid_;      // 地址 -> uuid
    // srcAddr(u64) -> (dstAddr(u64) -> 路径)
    std::unordered_map<uint64_t,
        std::unordered_map<uint64_t, PathInfo>> srcpaths_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> relay_pkts_{0};
    std::atomic<uint64_t> relay_bytes_{0};
};

} // namespace p2p

#endif // P2P_PROXY_H
