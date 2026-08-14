#ifndef P2P_SDK_TRANSPORT_UDP_SOCKET_H
#define P2P_SDK_TRANSPORT_UDP_SOCKET_H

// 非阻塞 UDP socket 封装：收发 + select 等待（客户端单线程事件循环用）

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace p2p {

inline bool sockaddr_from(const std::string& ip, uint16_t port, sockaddr_in& out) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &out.sin_addr) != 1) return false;
    out.sin_port = htons(port);
    return true;
}

inline std::string sockaddr_ip(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

inline uint16_t sockaddr_port(const sockaddr_in& a) { return ntohs(a.sin_port); }

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(uint16_t bind_port = 0, const char* bind_ip = nullptr) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port   = htons(bind_port);
        if (bind_ip) inet_pton(AF_INET, bind_ip, &bind_addr.sin_addr);
        else bind_addr.sin_addr.s_addr = INADDR_ANY;
        int on = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd_, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
            close();
            return false;
        }
        // 非阻塞
        int fl = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    // 等待可读，返回是否可读（超时毫秒，0=立即轮询）
    bool wait_readable(int timeout_ms) const {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_, &rfds);
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(fd_ + 1, &rfds, nullptr, nullptr,
                       timeout_ms >= 0 ? &tv : nullptr);
        return r > 0 && FD_ISSET(fd_, &rfds);
    }

    int recv_from(uint8_t* buf, int cap, sockaddr_in& from) const {
        socklen_t sl = sizeof(from);
        return (int)recvfrom(fd_, buf, (size_t)cap, 0, (sockaddr*)&from, &sl);
    }

    int send_to(const void* data, size_t len, const sockaddr_in& to) const {
        return (int)sendto(fd_, data, len, 0, (const sockaddr*)&to, sizeof(to));
    }

    uint16_t local_port() const {
        sockaddr_in a;
        socklen_t sl = sizeof(a);
        if (getsockname(fd_, (sockaddr*)&a, &sl) == 0) return ntohs(a.sin_port);
        return 0;
    }

    std::string local_ip() const {
        sockaddr_in a;
        socklen_t sl = sizeof(a);
        if (getsockname(fd_, (sockaddr*)&a, &sl) == 0) return sockaddr_ip(a);
        return "0.0.0.0";
    }

    int fd() const { return fd_; }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_ = -1;
};

} // namespace p2p

#endif // P2P_SDK_TRANSPORT_UDP_SOCKET_H
