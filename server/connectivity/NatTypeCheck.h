#ifndef P2P_NAT_TYPE_CHECK_H
#define P2P_NAT_TYPE_CHECK_H

#include "core/socket/Net.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>

namespace p2p {

// ---------------------------------------------------------------------------
// NAT 类型探测服务（对标原版 nattypecheck_server_start）
// 三 UDP socket：主 + 备用（过滤/映射）+ 第三探测口（NAT4E 步长）
// 客户端发 NAT_DETECT_REQ -> 主/备 socket 各回一条 NatDetectRsp：
//   - 若客户端 NAT 无端口过滤（锥型），备用 socket 的应答可送达
//   - 客户端再向备用 / 第三口各发一次，比较映射端口 → EIM/EDM + NAT4E
// ---------------------------------------------------------------------------
class NatTypeCheck {
public:
    NatTypeCheck();
    ~NatTypeCheck();

    // 创建主/备/探测 socket（均 INADDR_ANY；alt/probe=0 则临时端口）
    int  init(uint16_t main_port, uint16_t alt_port, uint16_t probe_port = 0);
    void request_stop() { running_ = false; }
    bool running() const { return running_; }

    // 备用 + 第三探测口快速路径（select 阻塞）
    void run_alt_thread();

    // 主 socket 快速路径（由 epoll 收包线程调用）：
    //   命中 NAT_DETECT_REQ -> 双 socket 应答并返回 true
    //   其它消息 -> 返回 false（交给处理线程池）
    bool try_fast_handle(const uint8_t* data, size_t len, const sockaddr_in& from);

    int  main_fd() const { return sock_main_.fd(); }
    int  alt_fd() const { return sock_alt_.fd(); }
    uint16_t alt_port() const { return alt_port_; }
    uint16_t probe_port() const { return probe_port_; }

private:
    // 对 from 主/备各回一条 NatDetectRsp（不含第三口，以免干扰过滤判定）
    void dual_reply(const sockaddr_in& from, uint8_t server_index_hint);
    void probe_reply(const sockaddr_in& from);

    UdpFd    sock_main_;    // RAII：析构自动关闭
    UdpFd    sock_alt_;
    UdpFd    sock_probe_;
    uint16_t main_port_ = 0;
    uint16_t alt_port_ = 0;
    uint16_t probe_port_ = 0;
    std::atomic<bool> running_{false};
};

} // namespace p2p

#endif // P2P_NAT_TYPE_CHECK_H
