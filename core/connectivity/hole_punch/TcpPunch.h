#ifndef P2P_COMMON_TCP_PUNCH_H
#define P2P_COMMON_TCP_PUNCH_H

// EasyTier 式 TCP 打洞：两端交换映射口后，从同一本地口同时 connect
// （RFC 793 simultaneous open）。对称 NAT（EDM）不发起；未知映射不发起。
// 不改 force_relay；默认关。禁止在 juice 回调里调用本文件的阻塞路径。

#include "core/socket/Net.h"
#include "core/socket/Packet.h"
#include "core/packet/ProtoDef.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace p2p {

inline bool tcp_punch_can_initiate(uint8_t mapping) {
    return mapping == NAT_MAP_EIM || mapping == NAT_MAP_ADM;
}

// listen + 同口 connect 需要 REUSEADDR+REUSEPORT（仅 REUSEADDR 在 Linux 会 bind 失败）
inline bool tcp_punch_reuse(int fd) {
    if (fd < 0) return false;
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) return false;
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    return true;
}

inline bool tcp_punch_listen(TcpFd& fd, uint16_t* port) {
    if (!port) return false;
    if (!fd.open() || !tcp_punch_reuse(fd.fd()) || !fd.bind_any(0) ||
        !fd.set_nonblock() || !fd.listen(4)) {
        fd.close();
        return false;
    }
    *port = fd.local_port();
    if (*port == 0) {
        fd.close();
        return false;
    }
    return true;
}

// 从 local_port 非阻塞 connect。返回 1=立刻通，0=EINPROGRESS，-1=失败。
inline int tcp_punch_connect_nb(TcpFd& fd, uint16_t local_port,
                                const char* ip, uint16_t port) {
    if (!ip || port == 0 || local_port == 0) return -1;
    if (!fd.open() || !tcp_punch_reuse(fd.fd()) || !fd.bind_any(local_port) ||
        !fd.set_nonblock() || !fd.set_nodelay()) {
        fd.close();
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fd.close();
        return -1;
    }
    const int r = ::connect(fd.fd(), reinterpret_cast<const sockaddr*>(&addr),
                            sizeof(addr));
    if (r == 0) return 1;
    if (errno == EINPROGRESS || errno == EALREADY) return 0;
    fd.close();
    return -1;
}

// 查询非阻塞 connect：1=已通，0=仍在进行，-1=失败。
inline int tcp_punch_connect_done(int sockfd) {
    if (sockfd < 0) return -1;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    if (err == 0) return 1;
    if (err == EINPROGRESS || err == EALREADY || err == EINTR) return 0;
    return -1;
}

inline size_t tcp_punch_frame(uint8_t* buf, size_t cap,
                              const void* payload, size_t plen) {
    return build_msg(buf, cap, MSG_TCP_DATA, payload, plen);
}

} // namespace p2p

#endif // P2P_COMMON_TCP_PUNCH_H
