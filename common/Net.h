#ifndef P2P_COMMON_NET_H
#define P2P_COMMON_NET_H

// 服务端 UDP socket 的 RAII 封装（C++ 风格重构基础设施）
//   - 所有权唯一（move-only），析构自动 close，杜绝 fd 泄漏
//   - 链式配置：open -> reuse -> bind
// 客户端 SDK 的非阻塞封装见 client/sdk/transport/UdpSocket.h（select 事件循环用）

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace p2p {

class UdpFd {
public:
    UdpFd() = default;
    explicit UdpFd(int fd) : fd_(fd) {}
    ~UdpFd() { close(); }

    UdpFd(const UdpFd&) = delete;
    UdpFd& operator=(const UdpFd&) = delete;
    UdpFd(UdpFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UdpFd& operator=(UdpFd&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }

    bool open() {
        close();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        return fd_ >= 0;
    }

    // SO_REUSEADDR（+可选 SO_REUSEPORT，多 socket 收包内核均衡分发用）
    bool set_reuse(bool reuse_port) {
        if (fd_ < 0) return false;
        int on = 1;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) return false;
        if (reuse_port &&
            setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) return false;
        return true;
    }

    bool bind_any(uint16_t port) {
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        return ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    ssize_t send_to(const void* data, size_t len, const sockaddr_in& to) const {
        return ::sendto(fd_, data, len, 0,
                        reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    ssize_t recv_from(void* buf, size_t cap, sockaddr_in& from) const {
        socklen_t fl = sizeof(from);
        return ::recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    }

    // 本地绑定端口（0 端口绑定后解析实际临时端口用）
    uint16_t local_port() const {
        sockaddr_in a{};
        socklen_t sl = sizeof(a);
        if (fd_ >= 0 && getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &sl) == 0)
            return ntohs(a.sin_port);
        return 0;
    }

    bool local_addr(sockaddr_in& out) const {
        socklen_t sl = sizeof(out);
        return fd_ >= 0 &&
               getsockname(fd_, reinterpret_cast<sockaddr*>(&out), &sl) == 0;
    }

    int  fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// TCP socket 的 RAII 封装（P2PTunnel 本地端口映射用）
class TcpFd {
public:
    TcpFd() = default;
    explicit TcpFd(int fd) : fd_(fd) {}
    ~TcpFd() { close(); }

    TcpFd(const TcpFd&) = delete;
    TcpFd& operator=(const TcpFd&) = delete;
    TcpFd(TcpFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    TcpFd& operator=(TcpFd&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }

    bool open() {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        return fd_ >= 0;
    }

    bool set_reuse() {
        if (fd_ < 0) return false;
        int on = 1;
        return setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
    }

    bool set_nodelay() {
        if (fd_ < 0) return false;
        int on = 1;
        return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
    }

    bool set_recv_timeout_ms(int ms) {
        if (fd_ < 0) return false;
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    bool bind_loopback(uint16_t port) {
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        return ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool listen(int backlog = 16) {
        return fd_ >= 0 && ::listen(fd_, backlog) == 0;
    }

    bool connect_to(const char* ip, uint16_t port) {
        if (fd_ < 0 || !ip) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
        return ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    // 成功返回已接管的新连接；失败返回无效对象
    TcpFd accept_one() {
        if (fd_ < 0) return TcpFd{};
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int nfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&from), &fl);
        return nfd >= 0 ? TcpFd(nfd) : TcpFd{};
    }

    ssize_t send_all(const void* data, size_t len) const {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::send(fd_, p + off, len - off, MSG_NOSIGNAL);
            if (n < 0) return n;
            if (n == 0) break;
            off += (size_t)n;
        }
        return (ssize_t)off;
    }

    ssize_t recv_some(void* buf, size_t cap) const {
        return ::recv(fd_, buf, cap, 0);
    }

    uint16_t local_port() const {
        sockaddr_in a{};
        socklen_t sl = sizeof(a);
        if (fd_ >= 0 && getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &sl) == 0)
            return ntohs(a.sin_port);
        return 0;
    }

    void shutdown_rw() {
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    }

    int  fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int  release() { return std::exchange(fd_, -1); }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// epoll 实例的 RAII 封装（单 fd 监听场景足够；多 fd 直接多次 add）
class EpollFd {
public:
    EpollFd() = default;
    ~EpollFd() { close(); }
    EpollFd(const EpollFd&) = delete;
    EpollFd& operator=(const EpollFd&) = delete;
    EpollFd(EpollFd&& o) noexcept : ep_(std::exchange(o.ep_, -1)) {}
    EpollFd& operator=(EpollFd&& o) noexcept {
        if (this != &o) {
            close();
            ep_ = std::exchange(o.ep_, -1);
        }
        return *this;
    }

    bool create() {
        close();
        ep_ = ::epoll_create1(0);
        return ep_ >= 0;
    }

    bool add_read(int fd) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        return ep_ >= 0 && ::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    int wait(epoll_event* events, int max_events, int timeout_ms) {
        return ::epoll_wait(ep_, events, max_events, timeout_ms);
    }

    bool valid() const { return ep_ >= 0; }

    void close() {
        if (ep_ >= 0) {
            ::close(ep_);
            ep_ = -1;
        }
    }

private:
    int ep_ = -1;
};

} // namespace p2p

#endif // P2P_COMMON_NET_H
