#include "server/connectivity/NatTypeCheck.h"
#include "core/foundation/Log.h"
#include "core/socket/Packet.h"
#include "core/packet/ProtoDef.h"
#include "core/foundation/Util.h"

#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>

namespace p2p {

NatTypeCheck::NatTypeCheck() {}
NatTypeCheck::~NatTypeCheck() {}   // socket 由 UdpFd RAII 关闭

static bool make_udp_socket(UdpFd& out, uint16_t port, const char* tag) {
    if (!out.open()) { perror(tag); return false; }
    if (!out.set_reuse(true) || !out.bind_any(port)) {
        perror(tag);
        out.close();
        return false;
    }
    LOGI("NatTypeCheck", "socket[%s] fd=%d port=%d", tag, out.fd(), port);
    return true;
}

int NatTypeCheck::init(uint16_t main_port, uint16_t alt_port, uint16_t probe_port) {
    main_port_ = main_port;
    alt_port_ = alt_port;       // 0 -> 临时端口（客户端通过应答获取实际端口）
    probe_port_ = probe_port;   // 0 -> 临时端口（NAT4E 第三口）
    if (!make_udp_socket(sock_main_, main_port, "[NatTypeCheck] main socket"))
        return -1;
    if (!make_udp_socket(sock_alt_, alt_port_, "[NatTypeCheck] alt socket"))
        return -1;
    if (alt_port_ == 0)
        alt_port_ = sock_alt_.local_port();
    if (!make_udp_socket(sock_probe_, probe_port_, "[NatTypeCheck] probe socket"))
        return -1;
    if (probe_port_ == 0)
        probe_port_ = sock_probe_.local_port();
    running_ = true;
    LOGI("NatTypeCheck", "main=%d alt=%d probe=%d", main_port_, alt_port_, probe_port_);
    return 0;
}

static void send_rsp(const UdpFd& sock, const sockaddr_in& to, uint8_t server_index,
                     uint16_t main_port, uint16_t alt_port, uint16_t probe_port) {
    NatDetectRsp rsp{};
    rsp.server_index = server_index;

    // 该 socket 的服务地址
    sockaddr_in self{};
    if (sock.local_addr(self)) {
        inet_ntop(AF_INET, &self.sin_addr, rsp.server_ip, MAX_IP_LEN);
        rsp.server_port = self.sin_port;
    } else {
        rsp.server_port = htons(0);
    }

    // 客户端在该 socket 上观察到的公网映射（即其源地址）
    inet_ntop(AF_INET, &to.sin_addr, rsp.mapped_ip, MAX_IP_LEN);
    rsp.mapped_port = to.sin_port;

    rsp.main_port = htons(main_port);
    rsp.alt_port = htons(alt_port);
    rsp.probe_port = htons(probe_port);

    uint8_t buf[sizeof(MsgHead) + sizeof(NatDetectRsp)];
    const size_t n = build_msg(buf, sizeof(buf), MSG_NAT_DETECT_RSP, &rsp, sizeof(rsp));
    if (n > 0) sock.send_to(buf, n, to);
}

void NatTypeCheck::dual_reply(const sockaddr_in& from, uint8_t server_index_hint) {
    // server_index_hint 标识请求落在哪个 socket，应答优先从该 socket 发出
    // 无论请求落在哪，都双 socket 各发一条，以便客户端做端口过滤检测
    (void)server_index_hint;
    send_rsp(sock_main_, from, 0, main_port_, alt_port_, probe_port_);
    send_rsp(sock_alt_, from, 1, main_port_, alt_port_, probe_port_);
}

void NatTypeCheck::probe_reply(const sockaddr_in& from) {
    send_rsp(sock_probe_, from, 2, main_port_, alt_port_, probe_port_);
}

bool NatTypeCheck::try_fast_handle(const uint8_t* data, size_t len, const sockaddr_in& from) {
    PacketReader r(data, len);
    MsgHead h{};
    if (!r.read_struct(h)) return false;
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return false;
    if (h.msg_id != MSG_NAT_DETECT_REQ) return false;
    dual_reply(from, 0);
    return true;
}

void NatTypeCheck::run_alt_thread() {
    fd_set rf;
    uint8_t buf[MAX_PKT];
    while (running_) {
        FD_ZERO(&rf);
        const int alt_fd = sock_alt_.fd();
        const int probe_fd = sock_probe_.fd();
        int maxfd = alt_fd;
        if (alt_fd >= 0) FD_SET(alt_fd, &rf);
        if (probe_fd >= 0) {
            FD_SET(probe_fd, &rf);
            if (probe_fd > maxfd) maxfd = probe_fd;
        }
        if (maxfd < 0) break;
        timeval tv{1, 0};
        int n = select(maxfd + 1, &rf, nullptr, nullptr, &tv);
        if (n < 0 && errno != EINTR) { perror("[NatTypeCheck] select"); break; }
        if (n == 0) continue;

        if (alt_fd >= 0 && FD_ISSET(alt_fd, &rf)) {
            sockaddr_in from{};
            ssize_t r = sock_alt_.recv_from(buf, sizeof(buf), from);
            if (r > 0) try_fast_handle(buf, (size_t)r, from);
        }
        if (probe_fd >= 0 && FD_ISSET(probe_fd, &rf)) {
            sockaddr_in from{};
            ssize_t r = sock_probe_.recv_from(buf, sizeof(buf), from);
            if (r <= 0) continue;
            PacketReader pr(buf, (size_t)r);
            MsgHead h{};
            if (!pr.read_struct(h)) continue;
            if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) continue;
            if (h.msg_id != MSG_NAT_DETECT_REQ) continue;
            probe_reply(from);
        }
    }
}

} // namespace p2p
