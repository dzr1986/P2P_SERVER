#ifndef P2P_PROXY_H
#define P2P_PROXY_H

#include "ProtoDef.h"
#include "ProxyRegistry.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// UDP 中继代理（TURN 类兜底，对标原版 proxyserver/src/P2PProxy.cpp）
//   - 代理注册：UUID/地址双索引 + 120 秒租约
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
    ProxyRegistry registry_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> relay_pkts_{0};
    std::atomic<uint64_t> relay_bytes_{0};
};

} // namespace p2p

#endif // P2P_PROXY_H
