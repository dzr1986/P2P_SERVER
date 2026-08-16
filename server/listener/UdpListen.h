#ifndef P2P_SERVER_LISTENER_UDP_LISTEN_H
#define P2P_SERVER_LISTENER_UDP_LISTEN_H

// EasyTier listener：只负责 bind/accept 端点，不处理业务报文。
// instance 的 recv_thread 消费这里产出的 socket。

#include "core/socket/Net.h"

#include <cstdint>

namespace p2p {

inline UdpFd open_reuseport_udp(uint16_t port) {
    UdpFd s;
    if (!s.open()) return s;
    if (!s.set_reuse(true) || !s.bind_any(port)) {
        s.close();
        return s;
    }
    return s;
}

}  // namespace p2p

#endif
