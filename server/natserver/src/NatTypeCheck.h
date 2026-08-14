#ifndef P2P_NAT_TYPE_CHECK_H
#define P2P_NAT_TYPE_CHECK_H

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>

namespace p2p {

// ---------------------------------------------------------------------------
// NAT 类型探测服务（对标原版 nattypecheck_server_start）
// 双 UDP socket：主 socket（g_sock）+ 备用 socket（g_another_sock）
// 客户端发 NAT_DETECT_REQ -> 主/备 socket 各回一条 NatDetectRsp：
//   - 若客户端 NAT 无端口过滤（锥型），备用 socket 的应答可送达
//   - 客户端再向备用 socket 发一次请求，可得到其在备用 socket 上的映射端口
//     与主 socket 映射端口比较即可区分 对称型/锥型
// ---------------------------------------------------------------------------
class NatTypeCheck {
public:
    NatTypeCheck();
    ~NatTypeCheck();

    // 创建主/备 socket，绑定主端口与备用端口（均 INADDR_ANY）
    int  init(uint16_t main_port, uint16_t alt_port);
    void request_stop() { running_ = false; }
    bool running() const { return running_; }

    // 备用 socket 快速路径线程（select 阻塞），处理发往备用端口的 NAT 探测
    void run_alt_thread();

    // 主 socket 快速路径（由 epoll 收包线程调用）：
    //   命中 NAT_DETECT_REQ -> 双 socket 应答并返回 true
    //   其它消息 -> 返回 false（交给处理线程池）
    bool try_fast_handle(const uint8_t* data, size_t len, const sockaddr_in& from);

    int  main_fd() const { return sock_main_; }
    int  alt_fd() const { return sock_alt_; }
    uint16_t alt_port() const { return alt_port_; }

private:
    // 对 from 双 socket 各回一条 NatDetectRsp
    void dual_reply(const sockaddr_in& from, uint8_t server_index_hint);

    int      sock_main_ = -1;
    int      sock_alt_ = -1;
    uint16_t main_port_ = 0;
    uint16_t alt_port_ = 0;
    std::atomic<bool> running_{false};
};

} // namespace p2p

#endif // P2P_NAT_TYPE_CHECK_H
