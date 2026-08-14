#include "NatTypeCheck.h"
#include "Log.h"
#include "ProtoDef.h"
#include "Util.h"

#include <cstring>
#include <sys/select.h>
#include <unistd.h>

namespace p2p {

NatTypeCheck::NatTypeCheck() {}
NatTypeCheck::~NatTypeCheck() {
    if (sock_main_ >= 0) close(sock_main_);
    if (sock_alt_ >= 0) close(sock_alt_);
}

static int make_udp_socket(uint16_t port, const char* tag) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror(tag); return -1; }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));  // 多收包线程共享端口
#endif
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        perror(tag);
        close(fd);
        return -1;
    }
    LOGI("NatTypeCheck", "socket[%s] fd=%d port=%d", tag, fd, port);
    return fd;
}

int NatTypeCheck::init(uint16_t main_port, uint16_t alt_port) {
    main_port_ = main_port;
    alt_port_ = alt_port;   // 0 -> 临时端口（客户端通过应答获取实际端口）
    sock_main_ = make_udp_socket(main_port, "[NatTypeCheck] main socket");
    if (sock_main_ < 0) return -1;
    sock_alt_ = make_udp_socket(alt_port_, "[NatTypeCheck] alt socket");
    if (sock_alt_ < 0) return -1;
    if (alt_port_ == 0) {   // 解析临时端口，供应答告知客户端
        sockaddr_in self;
        socklen_t sl = sizeof(self);
        if (getsockname(sock_alt_, (sockaddr*)&self, &sl) == 0)
            alt_port_ = ntohs(self.sin_port);
    }
    running_ = true;
    LOGI("NatTypeCheck", "main=%d alt=%d", main_port_, alt_port_);
    return 0;
}

static void send_rsp(int fd, const sockaddr_in& to, uint8_t server_index,
                     uint16_t main_port, uint16_t alt_port) {
    NatDetectRsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.server_index = server_index;

    // 该 socket 的服务地址
    sockaddr_in self;
    socklen_t sl = sizeof(self);
    if (getsockname(fd, (sockaddr*)&self, &sl) == 0) {
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

    uint8_t buf[MAX_PKT];
    MsgHead h;
    h.magic = htons(NAT_MAGIC);
    h.version = PROTO_VER;
    h.msg_id = MSG_NAT_DETECT_RSP;
    h.length = htonl(sizeof(rsp));
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &rsp, sizeof(rsp));
    sendto(fd, buf, sizeof(h) + sizeof(rsp), 0, (const sockaddr*)&to, sizeof(to));
}

void NatTypeCheck::dual_reply(const sockaddr_in& from, uint8_t server_index_hint) {
    // server_index_hint 标识请求落在哪个 socket，应答优先从该 socket 发出
    // 无论请求落在哪，都双 socket 各发一条，以便客户端做端口过滤检测
    (void)server_index_hint;
    send_rsp(sock_main_, from, 0, main_port_, alt_port_);
    send_rsp(sock_alt_, from, 1, main_port_, alt_port_);
}

bool NatTypeCheck::try_fast_handle(const uint8_t* data, size_t len, const sockaddr_in& from) {
    if (len < sizeof(MsgHead)) return false;
    MsgHead h;
    memcpy(&h, data, sizeof(h));
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
        FD_SET(sock_alt_, &rf);
        timeval tv{1, 0};
        int n = select(sock_alt_ + 1, &rf, nullptr, nullptr, &tv);
        if (n < 0 && errno != EINTR) { perror("[NatTypeCheck] select"); break; }
        if (n == 0) continue;

        sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(sock_alt_, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (r <= 0) continue;
        if (try_fast_handle(buf, (size_t)r, from)) continue;

        // 非 NAT 探测消息落到备用端口：忽略（备用端口专用快速路径）
    }
}

} // namespace p2p
