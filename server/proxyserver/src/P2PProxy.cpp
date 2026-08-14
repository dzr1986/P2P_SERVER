// P2PProxy.cpp：UDP 中继代理（多 socket 收包 + 双映射表 + 计数回收）
//   ./p2p_proxy <Port> <MaxProxyNum> [Workers]
// 功能：
//   - 代理注册（uuid->公网地址），建立源/目的双向中转路径（计数=3）
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

static constexpr time_t REG_TTL = 120;   // 注册表项 120s 无刷新回收
static constexpr time_t COUNT_TTL = 5;   // 计数衰减周期

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
    for (;;) {
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (r <= 0) {
            if (!running_) break;
            continue;
        }
        if (!running_) break;
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

    std::lock_guard<std::mutex> lk(mu_);

    // 满员拒绝（已注册的刷新不受限）
    if (uuid2addr_.size() >= max_proxy_ &&
        uuid2addr_.find(uuid) == uuid2addr_.end()) {
        rsp.result = 1;
        if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        LOGW("Proxy", "full, discard registration uuid[%s]", uuid.c_str());
        return;
    }

    // 同地址换 uuid：先清旧
    uint64_t key = addr_to_u64(from);
    auto ait = addr2uuid_.find(key);
    if (ait != addr2uuid_.end() && ait->second != uuid) {
        uuid2addr_.erase(ait->second);
    }

    UuidEntry e;
    e.addr = from;
    e.last_reg = time(nullptr);
    uuid2addr_[uuid] = e;
    addr2uuid_[key] = uuid;

    // 若目标 uuid 已注册，建立双向中转路径（计数=3）
    rsp.result = 0;
    if (with_rsp) {
        inet_ntop(AF_INET, &from.sin_addr, rsp.pub_ip, MAX_IP_LEN);
        rsp.pub_port = from.sin_port;
        send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
    }
    LOGI("Proxy", "register ok uuid[%s] addr[%s] used=%zu",
         uuid.c_str(), addr_to_str(from).c_str(), uuid2addr_.size());
}

void P2PProxy::unregister(const std::string& uuid, const sockaddr_in& from) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uuid2addr_.find(uuid);
    if (it == uuid2addr_.end()) return;
    if (!sockaddr_eq(it->second.addr, from)) return;   // 仅注册方可注销
    addr2uuid_.erase(addr_to_u64(it->second.addr));
    // 删除相关中转路径
    uint64_t ua = addr_to_u64(it->second.addr);
    srcpaths_.erase(ua);
    for (auto& kv : srcpaths_) kv.second.erase(ua);
    uuid2addr_.erase(it);
}

void P2PProxy::handle(uint8_t* data, size_t len, const sockaddr_in& from) {
    if (len < sizeof(MsgHead)) return;
    MsgHead h;
    memcpy(&h, data, sizeof(h));
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return;
    uint32_t hdr_len = ntohl(h.length);
    if (hdr_len > len - sizeof(MsgHead)) return;

    uint8_t* p = data + sizeof(MsgHead);
    size_t plen = hdr_len;

    switch (h.msg_id) {
    case MSG_PROXY_REGISTER_REQ: {
        if (plen < sizeof(ProxyRegReq)) return;
        ProxyRegReq req;
        memcpy(&req, p, sizeof(ProxyRegReq));
        req.uuid[MAX_UUID_LEN] = 0;
        do_register(req.uuid, from, true, req.hmac);
        break;
    }

    case MSG_PROXY_UNREGISTER_REQ: {
        ProxyRegReq req;
        memset(&req, 0, sizeof(req));
        if (plen >= sizeof(ProxyRegReq)) memcpy(&req, p, sizeof(ProxyRegReq));
        req.uuid[MAX_UUID_LEN] = 0;
        unregister(req.uuid, from);
        LOGI("Proxy", "unregister uuid[%s] used=%zu", req.uuid, uuid2addr_.size());
        break;
    }

    case MSG_PROXY_RELAY_DATA: {
        if (plen < sizeof(RelayFrame)) return;
        RelayFrame* frame = reinterpret_cast<RelayFrame*>(p);
        frame->src_uuid[MAX_UUID_LEN] = 0;
        frame->dst_uuid[MAX_UUID_LEN] = 0;

        sockaddr_in dst;
        uint64_t dst_key = 0;
        bool found = false;
        {
            // 合并原两次加锁为单次临界区：查目标地址并同时维护双向中转路径
            std::lock_guard<std::mutex> lk(mu_);
            auto it = uuid2addr_.find(frame->dst_uuid);
            if (it != uuid2addr_.end()) {
                dst = it->second.addr;
                dst_key = addr_to_u64(dst);
                found = true;
            }
            if (!found) {
                LOGD("Proxy", "relay drop, dst[%s] not registered", frame->dst_uuid);
                return;
            }
            // 源->目标 中转路径：命中则计数重置，未命中则创建
            uint64_t src_key = addr_to_u64(from);
            auto& m = srcpaths_[src_key];
            auto pit = m.find(dst_key);
            if (pit == m.end()) {
                PathInfo pi;
                pi.dst = dst;
                pi.count = 3;
                pi.uuid = frame->dst_uuid;
                pi.last_active = time(nullptr);
                m[dst_key] = pi;
            } else {
                pit->second.count = 3;
                pit->second.last_active = time(nullptr);
            }
            // 目标->源 反向路径（供回包）
            auto& m2 = srcpaths_[dst_key];
            auto pit2 = m2.find(src_key);
            if (pit2 == m2.end()) {
                PathInfo pi;
                pi.dst = from;
                pi.count = 3;
                pi.uuid = frame->src_uuid;
                pi.last_active = time(nullptr);
                m2[src_key] = pi;
            } else {
                pit2->second.count = 3;
                pit2->second.last_active = time(nullptr);
            }
        }

        // 原样转发整包（RelayFrame + TunnelFrame + 负载），已在锁外
        send(dst, MSG_PROXY_RELAY_DATA, p, plen);
        relay_pkts_.fetch_add(1);
        relay_bytes_.fetch_add(plen);
        break;
    }

    case MSG_PROXY_PUNCH_HELPER: {
        // 打洞协助：返回目标当前公网地址（对称 NAT 场景辅助）
        if (plen < sizeof(RelayFrame)) return;
        RelayFrame* frame = reinterpret_cast<RelayFrame*>(p);
        frame->dst_uuid[MAX_UUID_LEN] = 0;

        ProxyRegRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        std::lock_guard<std::mutex> lk(mu_);
        auto it = uuid2addr_.find(frame->dst_uuid);
        if (it == uuid2addr_.end()) {
            rsp.result = 1;
        } else {
            rsp.result = 0;
            inet_ntop(AF_INET, &it->second.addr.sin_addr, rsp.pub_ip, MAX_IP_LEN);
            rsp.pub_port = it->second.addr.sin_port;
        }
        send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
        break;
    }

    case MSG_SP_ASK_EXTINFO_REQ: {
        ProxyAvailRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        std::lock_guard<std::mutex> lk(mu_);
        rsp.available = (uuid2addr_.size() < max_proxy_) ? 1 : 0;
        rsp.used = htons((uint16_t)uuid2addr_.size());
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
        sleep(COUNT_TTL);
        time_t now = time(nullptr);
        // 缩小持锁粒度：先短锁内收集待回收键，再短锁内删除，避免全表扫描长期持锁
        std::vector<uint64_t> dead_src, dead_uuid;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = srcpaths_.begin(); it != srcpaths_.end();) {
                for (auto jt = it->second.begin(); jt != it->second.end();) {
                    if (jt->second.count > 0) jt->second.count--;
                    if (jt->second.count == 0 || now - jt->second.last_active > REG_TTL)
                        jt = it->second.erase(jt);
                    else
                        ++jt;
                }
                if (it->second.empty()) it = srcpaths_.erase(it); else ++it;
            }
            for (auto it = uuid2addr_.begin(); it != uuid2addr_.end();) {
                if (now - it->second.last_reg > REG_TTL) {
                    dead_uuid.push_back(addr_to_u64(it->second.addr));
                    it = uuid2addr_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!dead_uuid.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto k : dead_uuid) addr2uuid_.erase(k);
        }
        LOGD("Proxy", "tables: uuid=%zu srcpaths=%zu relay_pkts=%llu",
             uuid2addr_.size(), srcpaths_.size(),
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
        recv_threads.emplace_back(&P2PProxy::recv_loop, this, fd);
    }
    std::thread timer(&P2PProxy::timer_loop, this);

    LOGI("Proxy", "running with %zu recv sockets", fds_.size());
    while (running_) sleep(1);

    running_ = false;
    timer.join();
    for (size_t i = 0; i < recv_threads.size(); i++) {
        // 唤醒阻塞的 recvfrom：向每个 socket 发一个探测包
        sockaddr_in self;
        memset(&self, 0, sizeof(self));
        self.sin_family = AF_INET;
        self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        self.sin_port = htons(port_);
        sendto(fds_[i], "", 0, 0, (const sockaddr*)&self, sizeof(self));
        recv_threads[i].join();
        close(fds_[i]);
    }
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
