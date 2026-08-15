#ifndef P2P_COMMON_NET_H
#define P2P_COMMON_NET_H

// 服务端 UDP socket 的 RAII 封装（C++ 风格重构基础设施）
//   - 所有权唯一（move-only），析构自动 close，杜绝 fd 泄漏
//   - 链式配置：open -> reuse -> bind
// 客户端 SDK 的非阻塞封装见 client/sdk/transport/UdpSocket.h（select 事件循环用）

#include <arpa/inet.h>
#include <netinet/in.h>
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

} // namespace p2p

#endif // P2P_COMMON_NET_H
