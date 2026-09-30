// P2PProxy.cpp：UDP 中继代理（多 socket 收包 + 双索引注册表 + 租约回收）
//   ./p2p_proxy <Port> <MaxProxyNum> [Workers]
// 功能：
//   - 代理注册（uuid->公网地址），校验中继源地址与注册身份
//   - RELAY_DATA 查表转发（打洞失败兜底）
//   - PUNCH_HELPER 打洞协助：向请求方返回目标当前公网地址
//   - SP_ASK_EXTINFO_REQ 可用性查询（供 NatServer 择优调度）
#include "P2PProxy.h"
#include "Log.h"
#include "Util.h"
#include "Crypto.h"   // hmac_sha256

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace p2p {

P2PProxy::P2PProxy() {}
P2PProxy::~P2PProxy() {}

static constexpr unsigned CLEANUP_INTERVAL = 5;

// 代理注册鉴权共享密钥（演示用固定值；生产环境应从配置/环境变量注入）
static const uint8_t kProxyAuthKey[] = "p2p-proxy-auth-2024";
static constexpr size_t kProxyAuthKeyLen = sizeof(kProxyAuthKey) - 1;

int P2PProxy::init(uint16_t port, uint16_t max_proxy, int workers) {
    port_ = port;
    max_proxy_ = max_proxy;
    workers_ = (workers >= 1 && workers <= 64) ? workers : 4;

    // 预检端口可用（SO_REUSEPORT 多 socket 场景由 recv_loop 创建）
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) { perror("[Proxy] socket"); return -1; }
    int on = 1;
    setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (bind(probe, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("[Proxy] bind");
        close(probe);
        return -1;
    }
    close(probe);

    LOGI("Proxy", "start proxy server with Port[%d] maxProxy[%d] workers[%d]",
         port_, max_proxy_, workers_);
    return 0;
}

void P2PProxy::recv_loop(int fd) {
    uint8_t buf[MAX_PKT];
    while (running_) {
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(fd, buf, sizeof(buf), MSG_TRUNC, (sockaddr*)&from, &fl);
        if (r <= 0) {
            if (!running_) break;
            continue;
        }
        if (!running_) break;
        if (static_cast<size_t>(r) > sizeof(buf)) continue;
        handle(buf, (size_t)r, from);
    }
}

int P2PProxy::send(const sockaddr_in& to, uint8_t msg_id,
                   const void* payload, size_t plen) {
    int fd = out_fd();
    if (fd < 0) return -1;
    uint8_t buf[MAX_PKT];
    if (plen > sizeof(buf) - sizeof(MsgHead)) return -1;
    MsgHead h;
    h.magic = htons(NAT_MAGIC);
    h.version = PROTO_VER;
    h.msg_id = msg_id;
    h.length = htonl((uint32_t)plen);
    memcpy(buf, &h, sizeof(h));
    if (plen) memcpy(buf + sizeof(h), payload, plen);
    return (int)sendto(fd, buf, sizeof(h) + plen, 0, (const sockaddr*)&to, sizeof(to));
}

void P2PProxy::do_register(const std::string& uuid, const sockaddr_in& from,
                           bool with_rsp, const uint8_t* hmac) {
    ProxyRegRsp rsp;
    memset(&rsp, 0, sizeof(rsp));

    if (uuid.empty()) {
        rsp.result = 2;
        if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        return;
    }

    // HMAC 鉴权：防止伪造 uuid 注册（参考 peerko 的共享密钥校验实践）
    if (hmac == nullptr) {
        rsp.result = 3;
        if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        LOGW("Proxy", "register rejected, no hmac uuid[%s]", uuid.c_str());
        return;
    }
    uint8_t expect[32];
    hmac_sha256(kProxyAuthKey, kProxyAuthKeyLen,
                reinterpret_cast<const uint8_t*>(uuid.data()), uuid.size(), expect);
    if (memcmp(expect, hmac, 32) != 0) {
        rsp.result = 3;
        if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        LOGW("Proxy", "register rejected, bad hmac uuid[%s]", uuid.c_str());
        return;
    }

    size_t used;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rsp.result = registry_.register_peer(uuid, from, max_proxy_,
                                            ProxyRegistry::Clock::now()) ? 0 : 1;
        used = registry_.size();
    }
    if (rsp.result == 0) {
        inet_ntop(AF_INET, &from.sin_addr, rsp.pub_ip, MAX_IP_LEN);
        rsp.pub_port = from.sin_port;
    }
    if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
    LOGI("Proxy", "register %s uuid[%s] addr[%s] used=%zu",
         rsp.result == 0 ? "ok" : "full", uuid.c_str(), addr_to_str(from).c_str(), used);
}

void P2PProxy::unregister(const std::string& uuid, const sockaddr_in& from) {
    size_t used;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!registry_.unregister_peer(uuid, from)) return;
        used = registry_.size();
    }
    LOGI("Proxy", "unregister uuid[%s] used=%zu", uuid.c_str(), used);
}

void P2PProxy::handle(uint8_t* data, size_t len, const sockaddr_in& from) {
    if (len < sizeof(MsgHead)) return;
    MsgHead h;
    memcpy(&h, data, sizeof(h));
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return;
    uint32_t hdr_len = ntohl(h.length);
    if (hdr_len != len - sizeof(MsgHead)) return;

    uint8_t* p = data + sizeof(MsgHead);
    size_t plen = hdr_len;

    switch (h.msg_id) {
    case MSG_PROXY_REGISTER_REQ: {
        if (plen != sizeof(ProxyRegReq)) return;
        ProxyRegReq req;
        memcpy(&req, p, sizeof(ProxyRegReq));
        if (!memchr(req.uuid, 0, sizeof(req.uuid))) return;
        do_register(req.uuid, from, true, req.hmac);
        break;
    }

    case MSG_PROXY_UNREGISTER_REQ: {
        if (plen != sizeof(ProxyRegReq)) return;
        ProxyRegReq req;
        memcpy(&req, p, sizeof(req));
        if (!memchr(req.uuid, 0, sizeof(req.uuid))) return;
        unregister(req.uuid, from);
        break;
    }

    case MSG_PROXY_RELAY_DATA: {
        if (plen < sizeof(RelayFrame)) return;
        RelayFrame frame;
        memcpy(&frame, p, sizeof(frame));
        if (!memchr(frame.src_uuid, 0, sizeof(frame.src_uuid)) ||
            !memchr(frame.dst_uuid, 0, sizeof(frame.dst_uuid))) return;

        sockaddr_in dst;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!registry_.resolve(frame.src_uuid, from, frame.dst_uuid, dst,
                                   ProxyRegistry::Clock::now())) return;
        }

        // 原样转发整包（RelayFrame + TunnelFrame + 负载），已在锁外
        if (send(dst, MSG_PROXY_RELAY_DATA, p, plen) >= 0) {
            relay_pkts_.fetch_add(1, std::memory_order_relaxed);
            relay_bytes_.fetch_add(plen, std::memory_order_relaxed);
        }
        break;
    }

    case MSG_PROXY_PUNCH_HELPER: {
        // 打洞协助：返回目标当前公网地址（对称 NAT 场景辅助）
        if (plen != sizeof(RelayFrame)) return;
        RelayFrame frame;
        memcpy(&frame, p, sizeof(frame));
        if (!memchr(frame.src_uuid, 0, sizeof(frame.src_uuid)) ||
            !memchr(frame.dst_uuid, 0, sizeof(frame.dst_uuid))) return;

        ProxyRegRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        sockaddr_in dst;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto now = ProxyRegistry::Clock::now();
            if (!registry_.registered(frame.src_uuid, from, now)) return;
            rsp.result = registry_.resolve(frame.src_uuid, from, frame.dst_uuid, dst, now) ? 0 : 1;
        }
        if (rsp.result == 0) {
            inet_ntop(AF_INET, &dst.sin_addr, rsp.pub_ip, MAX_IP_LEN);
            rsp.pub_port = dst.sin_port;
        }
        send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        break;
    }

    case MSG_SP_ASK_EXTINFO_REQ: {
        ProxyAvailRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        {
            std::lock_guard<std::mutex> lk(mu_);
            rsp.available = (registry_.size() < max_proxy_) ? 1 : 0;
            rsp.used = htons((uint16_t)registry_.size());
        }
        rsp.max_proxy = htons(max_proxy_);
        send(from, MSG_SP_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
        break;
    }

    default:
        LOGW("Proxy", "unsupported msg 0x%02x from [%s]",
             h.msg_id, addr_to_str(from).c_str());
        break;
    }
}

void P2PProxy::timer_loop() {
    while (running_) {
        sleep(CLEANUP_INTERVAL);
        size_t used;
        {
            std::lock_guard<std::mutex> lk(mu_);
            registry_.expire(ProxyRegistry::Clock::now());
            used = registry_.size();
        }
        LOGD("Proxy", "tables: uuid=%zu relay_pkts=%llu", used,
             (unsigned long long)relay_pkts_.load());
    }
}

void P2PProxy::run() {
    running_ = true;
    std::vector<std::thread> recv_threads;
    fds_.clear();
    for (int i = 0; i < workers_; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("[Proxy] socket"); continue; }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        timeval timeout{0, 200000};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            perror("[Proxy] receive timeout");
            close(fd);
            continue;
        }
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) != 0) {
            perror("[Proxy] bind");
            close(fd);
            continue;
        }
        fds_.push_back(fd);
    }
    if (fds_.empty()) { running_ = false; return; }
    // 所有线程共享发送 socket；先完成容器初始化，再启动收包线程。
    for (int fd : fds_) recv_threads.emplace_back(&P2PProxy::recv_loop, this, fd);
    std::thread timer(&P2PProxy::timer_loop, this);

    LOGI("Proxy", "running with %zu recv sockets", fds_.size());
    while (running_) sleep(1);

    running_ = false;
    timer.join();
    for (auto& thread : recv_threads) thread.join();
    for (int fd : fds_) close(fd);
    fds_.clear();
    LOGI("Proxy", "stopped, relay_pkts=%llu relay_bytes=%llu",
         (unsigned long long)relay_pkts_.load(),
         (unsigned long long)relay_bytes_.load());
}

} // namespace p2p

// 独立 main：生成单例并启动
int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <Port> <MaxProxyNum> [Workers]\n", argv[0]);
        return 1;
    }
    int workers = argc >= 4 ? atoi(argv[3]) : 4;
    static p2p::P2PProxy proxy;
    if (proxy.init((uint16_t)atoi(argv[1]), (uint16_t)atoi(argv[2]), workers) != 0)
        return 1;
    proxy.run();
    return 0;
}
