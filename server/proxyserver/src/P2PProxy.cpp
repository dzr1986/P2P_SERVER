// P2PProxy.cpp：UDP 中继代理（多 socket 收包 + 双映射表 + 计数回收）
//   ./p2p_proxy <Port> <MaxProxyNum> [Workers]
// 功能：
//   - 代理注册（uuid->公网地址），建立源/目的双向中转路径（计数=3）
//   - RELAY_DATA 查表转发（打洞失败兜底）
//   - PUNCH_HELPER 打洞协助：向请求方返回目标当前公网地址
//   - SP_ASK_EXTINFO_REQ 可用性查询（供 NatServer 择优调度）
#include "P2PProxy.h"
#include "Crypto.h"   // hmac_sha256 / p2p_const_time_eq
#include "Log.h"
#include "Packet.h"
#include "Util.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace p2p {

namespace {

constexpr time_t kRegTtl = 120;    // 注册表项 120s 无刷新回收
constexpr time_t kCountTtl = 3;    // 计数衰减周期（缩短以控制 srcpaths 膨胀）
constexpr uint32_t kPathInitCount = 3;

// 代理注册鉴权共享密钥（演示用固定值；生产环境应从配置/环境变量注入）
constexpr uint8_t kProxyAuthKey[] = "p2p-proxy-auth-2024";
constexpr size_t  kProxyAuthKeyLen = sizeof(kProxyAuthKey) - 1;

} // namespace

int P2PProxy::init(uint16_t port, uint16_t max_proxy, int workers,
                   uint64_t quota_bytes, uint16_t alt_port, uint16_t tcp_port) {
    port_ = port;
    alt_port_ = (alt_port && alt_port != port) ? alt_port : 0;
    // 未单独给 TcpPort 时，AltPort（生产 443）兼听 TCP —— 对标 DERP 同口 UDP+TLS
    if (tcp_port && tcp_port != port) tcp_port_ = tcp_port;
    else if (!tcp_port && alt_port_) tcp_port_ = alt_port_;
    else tcp_port_ = 0;
    max_proxy_ = max_proxy;
    workers_ = (workers >= 1 && workers <= 64) ? workers : 4;
    quota_bytes_ = quota_bytes;

    // 预检端口可用（SO_REUSEPORT 多 socket 场景由 run 创建）
    UdpFd probe;
    if (!probe.open()) { perror("[Proxy] socket"); return -1; }
    if (!probe.set_reuse(false) || !probe.bind_any(port_)) {
        perror("[Proxy] bind");
        return -1;
    }
    if (alt_port_) {
        UdpFd alt;
        if (!alt.open()) { perror("[Proxy] alt socket"); return -1; }
        if (!alt.set_reuse(false) || !alt.bind_any(alt_port_)) {
            perror("[Proxy] bind alt");
            return -1;
        }
    }

    if (tcp_port_) {
        if (!tcp_listen_.open() || !tcp_listen_.set_reuse() ||
            !tcp_listen_.bind_any(tcp_port_) || !tcp_listen_.listen(128)) {
            perror("[Proxy] tcp listen");
            return -1;
        }
        tcp_listen_.set_nodelay();
        const char* cert = getenv("P2P_PROXY_TLS_CERT");
        const char* key  = getenv("P2P_PROXY_TLS_KEY");
        // 未给证书也启 TLS（临时自签），可用 P2P_PROXY_TLS=0 退回明文 TCP
        const char* tls_off = getenv("P2P_PROXY_TLS");
        const bool want_tls = !(tls_off && tls_off[0] == '0');
        if (want_tls) {
            if (!tls_make_server_ctx(tls_ctx_, cert, key)) {
                LOGW("Proxy", "TLS ctx failed, DERP tcp will be plaintext");
            } else {
                tls_enabled_ = true;
            }
        }
    }

    LOGI("Proxy", "start proxy server with Port[%d] altPort[%d] tcpPort[%d] tls=%d maxProxy[%d] workers[%d] quota=%llu",
         port_, alt_port_, tcp_port_, tls_enabled_ ? 1 : 0, max_proxy_, workers_,
         (unsigned long long)quota_bytes_);
    return 0;
}

void P2PProxy::recv_loop(const UdpFd& sock) {
    uint8_t buf[MAX_PKT];
    for (;;) {
        sockaddr_in from{};
        ssize_t r = sock.recv_from(buf, sizeof(buf), from);
        if (!running_) break;
        if (r <= 0) continue;
        handle(buf, (size_t)r, from);
    }
}

int P2PProxy::send(const sockaddr_in& to, uint8_t msg_id,
                   const void* payload, size_t plen) {
    if (socks_.empty()) return -1;
    const UdpFd& out = socks_.front();

    uint8_t buf[MAX_PKT];
    const size_t total = build_msg(buf, sizeof(buf), msg_id, payload, plen);
    if (total == 0) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        ssize_t n = out.send_to(buf, total, to);
        if (n >= 0) return (int)n;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
    }
    LOGW("Proxy", "sendto failed msg=0x%02x dest[%s] errno=%d",
         msg_id, addr_to_str(to).c_str(), errno);
    return -1;
}

// ---------------------------------------------------------------------------
// 路径表
// ---------------------------------------------------------------------------
void P2PProxy::touch_path(uint64_t src_key, uint64_t dst_key,
                          const sockaddr_in& dst, const std::string& uuid) {
    auto& sh = path_shards_[shard_of(src_key)];
    std::lock_guard<std::mutex> lk(sh.mu);
    auto& m = sh.paths[src_key];
    auto it = m.find(dst_key);
    if (it != m.end()) {
        it->second.count = kPathInitCount;
        it->second.last_active = time(nullptr);
        return;
    }
    if (m.size() >= kMaxPathsPerSrc) {
        // 超上限：淘汰最旧路径
        auto oldest = m.begin();
        for (auto jt = m.begin(); jt != m.end(); ++jt) {
            if (jt->second.last_active < oldest->second.last_active) oldest = jt;
        }
        m.erase(oldest);
    }
    PathInfo pi;
    pi.dst = dst;
    pi.count = kPathInitCount;
    pi.uuid = uuid;
    pi.last_active = time(nullptr);
    m.emplace(dst_key, std::move(pi));
}

void P2PProxy::erase_paths_for(uint64_t addr_key) {
    {
        auto& sh = path_shards_[shard_of(addr_key)];
        std::lock_guard<std::mutex> lk(sh.mu);
        sh.paths.erase(addr_key);
    }
    for (int i = 0; i < kPathShards; i++) {
        if (i == shard_of(addr_key)) continue;
        std::lock_guard<std::mutex> lk(path_shards_[i].mu);
        for (auto& kv : path_shards_[i].paths) kv.second.erase(addr_key);
    }
}

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------
void P2PProxy::do_register(const std::string& uuid, const sockaddr_in& from,
                           bool with_rsp, const uint8_t* hmac) {
    ProxyRegRsp rsp{};
    auto reply = [&] {
        if (with_rsp) send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
    };

    if (uuid.empty()) {
        rsp.result = 2;
        reply();
        return;
    }

    // HMAC 鉴权：防止伪造 uuid 注册（参考 peerko 的共享密钥校验实践）
    if (hmac == nullptr) {
        rsp.result = 3;
        reply();
        LOGW("Proxy", "register rejected, no hmac uuid[%s]", uuid.c_str());
        return;
    }
    uint8_t expect[32];
    hmac_sha256(kProxyAuthKey, kProxyAuthKeyLen,
                reinterpret_cast<const uint8_t*>(uuid.data()), uuid.size(), expect);
    if (!p2p_const_time_eq(expect, hmac, 32)) {
        rsp.result = 3;
        reply();
        LOGW("Proxy", "register rejected, bad hmac uuid[%s]", uuid.c_str());
        return;
    }

    std::unique_lock<std::shared_mutex> lk(reg_mu_);

    // 满员拒绝（已注册的刷新不受限）
    if (uuid2addr_.size() >= max_proxy_ &&
        uuid2addr_.find(uuid) == uuid2addr_.end()) {
        rsp.result = 1;
        reply();
        LOGW("Proxy", "full, discard registration uuid[%s]", uuid.c_str());
        return;
    }

    // 同地址换 uuid：先清旧
    const uint64_t key = addr_to_u64(from);
    auto ait = addr2uuid_.find(key);
    if (ait != addr2uuid_.end() && ait->second != uuid) {
        uuid2addr_.erase(ait->second);
    }

    UuidEntry e;
    e.addr = from;
    e.last_reg = time(nullptr);
    uuid2addr_[uuid] = e;
    addr2uuid_[key] = uuid;

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
    uint64_t ua = 0;
    {
        std::unique_lock<std::shared_mutex> lk(reg_mu_);
        auto it = uuid2addr_.find(uuid);
        if (it == uuid2addr_.end()) return;
        if (!sockaddr_eq(it->second.addr, from)) return;   // 仅注册方可注销
        ua = addr_to_u64(it->second.addr);
        addr2uuid_.erase(ua);
        uuid2addr_.erase(it);
    }
    erase_paths_for(ua);
}

// ---------------------------------------------------------------------------
// 报文分发与消息处理器
// ---------------------------------------------------------------------------
void P2PProxy::handle(uint8_t* data, size_t len, const sockaddr_in& from) {
    PacketReader r(data, len);
    MsgHead h{};
    if (!r.read_struct(h)) return;
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return;
    const uint32_t body_len = ntohl(h.length);
    if (body_len > r.remaining()) return;

    uint8_t* p = data + sizeof(MsgHead);
    const size_t plen = body_len;

    switch (h.msg_id) {
    case MSG_PROXY_REGISTER_REQ:   on_register_req(p, plen, from); break;
    case MSG_PROXY_UNREGISTER_REQ: on_unregister_req(p, plen, from); break;
    case MSG_PROXY_RELAY_DATA:     on_relay_data(p, plen, from); break;
    case MSG_PROXY_PUNCH_HELPER:   on_punch_helper(p, plen, from); break;
    case MSG_SP_ASK_EXTINFO_REQ:   on_avail_query(from); break;
    default:
        LOGW("Proxy", "unsupported msg 0x%02x from [%s]",
             h.msg_id, addr_to_str(from).c_str());
        break;
    }
}

void P2PProxy::on_register_req(const uint8_t* p, size_t plen, const sockaddr_in& from) {
    PacketReader r(p, plen);
    ProxyRegReq req{};
    if (!r.read_struct(req)) return;
    do_register(wire_str(req.uuid), from, true, req.hmac);
}

void P2PProxy::on_unregister_req(const uint8_t* p, size_t plen, const sockaddr_in& from) {
    PacketReader r(p, plen);
    ProxyRegReq req{};
    if (!r.read_struct(req)) return;   // 长度不足视为非法注销请求
    const std::string uuid = wire_str(req.uuid);
    unregister(uuid, from);
    LOGI("Proxy", "unregister uuid[%s] used=%zu", uuid.c_str(), registered_count());
}

void P2PProxy::on_relay_data(uint8_t* p, size_t plen, const sockaddr_in& from) {
    if (plen < sizeof(RelayFrame)) return;
    auto* frame = reinterpret_cast<RelayFrame*>(p);
    frame->src_uuid[MAX_UUID_LEN] = 0;
    frame->dst_uuid[MAX_UUID_LEN] = 0;

    sockaddr_in dst{};
    uint64_t dst_key = 0;
    bool have_udp_dst = false;
    {
        std::shared_lock<std::shared_mutex> lk(reg_mu_);
        auto sit = uuid2addr_.find(frame->src_uuid);
        if (sit == uuid2addr_.end() || !sockaddr_eq(sit->second.addr, from)) {
            LOGD("Proxy", "relay drop, src[%s] not registered or spoofed",
                 frame->src_uuid);
            return;
        }
        auto dit = uuid2addr_.find(frame->dst_uuid);
        if (dit != uuid2addr_.end()) {
            dst = dit->second.addr;
            dst_key = addr_to_u64(dst);
            have_udp_dst = true;
        }
    }
    {
        std::lock_guard<std::mutex> tlk(tcp_mu_);
        const bool have_tcp_dst = uuid2tcp_.count(frame->dst_uuid) > 0;
        if (!have_udp_dst && !have_tcp_dst) {
            LOGD("Proxy", "relay drop, dst[%s] not registered", frame->dst_uuid);
            return;
        }
    }
    if (quota_bytes_ > 0) {
        std::lock_guard<std::mutex> qlk(quota_mu_);
        uint64_t& used = quota_used_[frame->src_uuid];
        if (used >= quota_bytes_) {
            relay_drop_quota_.fetch_add(1);
            LOGW("Proxy", "relay quota exceeded uuid[%s] used=%llu cap=%llu",
                 frame->src_uuid, (unsigned long long)used,
                 (unsigned long long)quota_bytes_);
            return;
        }
        used += plen;
    }
    if (have_udp_dst) {
        const uint64_t src_key = addr_to_u64(from);
        touch_path(src_key, dst_key, dst, frame->dst_uuid);
        touch_path(dst_key, src_key, from, frame->src_uuid);
    }
    // 优先 TCP/TLS（DERP）；没有再走 UDP
    if (forward_relay(frame->dst_uuid, p, plen, frame->src_uuid)) {
        relay_pkts_.fetch_add(1);
        relay_bytes_.fetch_add(plen);
        return;
    }
    if (have_udp_dst) send(dst, MSG_PROXY_RELAY_DATA, p, plen);
    relay_pkts_.fetch_add(1);
    relay_bytes_.fetch_add(plen);
}

void P2PProxy::on_punch_helper(uint8_t* p, size_t plen, const sockaddr_in& from) {
    // 打洞协助：返回目标当前公网地址（对称 NAT 场景辅助）
    if (plen < sizeof(RelayFrame)) return;
    auto* frame = reinterpret_cast<RelayFrame*>(p);
    frame->dst_uuid[MAX_UUID_LEN] = 0;

    ProxyRegRsp rsp{};
    {
        std::shared_lock<std::shared_mutex> lk(reg_mu_);
        auto it = uuid2addr_.find(frame->dst_uuid);
        if (it == uuid2addr_.end()) {
            rsp.result = 1;
        } else {
            rsp.result = 0;
            inet_ntop(AF_INET, &it->second.addr.sin_addr, rsp.pub_ip, MAX_IP_LEN);
            rsp.pub_port = it->second.addr.sin_port;
        }
    }
    send(from, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp));
}

int P2PProxy::send_tcp(TcpClient& cli, uint8_t msg_id,
                       const void* payload, size_t plen) {
    uint8_t buf[MAX_PKT];
    const size_t total = build_msg(buf, sizeof(buf), msg_id, payload, plen);
    if (total == 0 || !cli.alive.load()) return -1;
    if (cli.use_tls) {
        return (int)cli.tls.write(buf, total);
    }
    return (int)cli.fd.send_all(buf, total);
}

bool P2PProxy::forward_relay(const std::string& dst_uuid, uint8_t* p, size_t plen,
                             const std::string& /*src_uuid*/) {
    std::shared_ptr<TcpClient> dst;
    {
        std::lock_guard<std::mutex> lk(tcp_mu_);
        auto it = uuid2tcp_.find(dst_uuid);
        if (it == uuid2tcp_.end() || !it->second || !it->second->alive.load())
            return false;
        dst = it->second;
    }
    return send_tcp(*dst, MSG_PROXY_RELAY_DATA, p, plen) > 0;
}

void P2PProxy::do_register_tcp(const std::string& uuid, std::shared_ptr<TcpClient> cli,
                              const uint8_t* hmac) {
    ProxyRegRsp rsp{};
    auto reply = [&] { send_tcp(*cli, MSG_PROXY_REGISTER_RSP, &rsp, sizeof(rsp)); };

    if (uuid.empty() || !cli) {
        rsp.result = 2;
        reply();
        return;
    }
    if (hmac == nullptr) {
        rsp.result = 3;
        reply();
        return;
    }
    uint8_t expect[32];
    hmac_sha256(kProxyAuthKey, kProxyAuthKeyLen,
                reinterpret_cast<const uint8_t*>(uuid.data()), uuid.size(), expect);
    if (!p2p_const_time_eq(expect, hmac, 32)) {
        rsp.result = 3;
        reply();
        LOGW("Proxy", "tcp register rejected, bad hmac uuid[%s]", uuid.c_str());
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lk(reg_mu_);
        if (uuid2addr_.size() >= max_proxy_ &&
            uuid2addr_.find(uuid) == uuid2addr_.end()) {
            std::lock_guard<std::mutex> tlk(tcp_mu_);
            if (uuid2tcp_.find(uuid) == uuid2tcp_.end()) {
                rsp.result = 1;
                reply();
                LOGW("Proxy", "full, discard tcp registration uuid[%s]", uuid.c_str());
                return;
            }
        }
    }

    cli->uuid = uuid;
    {
        std::lock_guard<std::mutex> tlk(tcp_mu_);
        uuid2tcp_[uuid] = cli;
    }
    rsp.result = 0;
    inet_ntop(AF_INET, &cli->peer.sin_addr, rsp.pub_ip, MAX_IP_LEN);
    rsp.pub_port = cli->peer.sin_port;
    reply();
    LOGI("Proxy", "register ok tcp uuid[%s] addr[%s] tls=%d",
         uuid.c_str(), addr_to_str(cli->peer).c_str(), cli->use_tls ? 1 : 0);
}

void P2PProxy::handle_tcp(std::shared_ptr<TcpClient> cli, uint8_t msg_id,
                         uint8_t* p, size_t plen) {
    if (!cli) return;
    switch (msg_id) {
    case MSG_PROXY_REGISTER_REQ: {
        PacketReader r(p, plen);
        ProxyRegReq req{};
        if (!r.read_struct(req)) return;
        do_register_tcp(wire_str(req.uuid), cli, req.hmac);
        break;
    }
    case MSG_PROXY_UNREGISTER_REQ: {
        PacketReader r(p, plen);
        ProxyRegReq req{};
        if (!r.read_struct(req)) return;
        const std::string uuid = wire_str(req.uuid);
        std::lock_guard<std::mutex> tlk(tcp_mu_);
        auto it = uuid2tcp_.find(uuid);
        if (it != uuid2tcp_.end() && it->second == cli) uuid2tcp_.erase(it);
        LOGI("Proxy", "unregister tcp uuid[%s]", uuid.c_str());
        break;
    }
    case MSG_PROXY_RELAY_DATA: {
        if (plen < sizeof(RelayFrame)) return;
        auto* frame = reinterpret_cast<RelayFrame*>(p);
        frame->src_uuid[MAX_UUID_LEN] = 0;
        frame->dst_uuid[MAX_UUID_LEN] = 0;
        if (cli->uuid.empty() || cli->uuid != frame->src_uuid) {
            LOGD("Proxy", "tcp relay drop, src mismatch sess[%s] frame[%s]",
                 cli->uuid.c_str(), frame->src_uuid);
            return;
        }
        if (quota_bytes_ > 0) {
            std::lock_guard<std::mutex> qlk(quota_mu_);
            uint64_t& used = quota_used_[frame->src_uuid];
            if (used >= quota_bytes_) {
                relay_drop_quota_.fetch_add(1);
                return;
            }
            used += plen;
        }
        if (forward_relay(frame->dst_uuid, p, plen, frame->src_uuid)) {
            relay_pkts_.fetch_add(1);
            relay_bytes_.fetch_add(plen);
            return;
        }
        sockaddr_in dst{};
        bool have_udp = false;
        {
            std::shared_lock<std::shared_mutex> lk(reg_mu_);
            auto dit = uuid2addr_.find(frame->dst_uuid);
            if (dit != uuid2addr_.end()) {
                dst = dit->second.addr;
                have_udp = true;
            }
        }
        if (have_udp) send(dst, MSG_PROXY_RELAY_DATA, p, plen);
        relay_pkts_.fetch_add(1);
        relay_bytes_.fetch_add(plen);
        break;
    }
    case MSG_PROXY_PUNCH_HELPER:
        on_punch_helper(p, plen, cli->peer);
        break;
    default:
        break;
    }
}

void P2PProxy::tcp_session_loop(std::shared_ptr<TcpClient> cli) {
    if (!cli) return;
    uint8_t tmp[MAX_PKT];
    while (running_.load() && cli->alive.load()) {
        ssize_t n = 0;
        if (cli->use_tls) n = cli->tls.read(tmp, sizeof(tmp));
        else n = cli->fd.recv_some(tmp, sizeof(tmp));
        if (n <= 0) break;
        cli->rbuf.insert(cli->rbuf.end(), tmp, tmp + n);
        for (;;) {
            uint8_t msg_id = 0;
            std::vector<uint8_t> payload;
            int c = xn_pop_frame(cli->rbuf, msg_id, payload);
            if (c == 0) break;
            if (c < 0) { cli->alive.store(false); break; }
            handle_tcp(cli, msg_id, payload.data(), payload.size());
        }
    }
    cli->alive.store(false);
    if (!cli->uuid.empty()) {
        std::lock_guard<std::mutex> tlk(tcp_mu_);
        auto it = uuid2tcp_.find(cli->uuid);
        if (it != uuid2tcp_.end() && it->second == cli) uuid2tcp_.erase(it);
    }
    LOGI("Proxy", "DERP tcp session closed uuid[%s]", cli->uuid.c_str());
}

void P2PProxy::tcp_accept_loop() {
    LOGI("Proxy", "DERP tcp listen %d tls=%d", tcp_port_, tls_enabled_ ? 1 : 0);
    while (running_.load()) {
        TcpFd nfd = tcp_listen_.accept_one();
        if (!nfd.valid()) {
            if (!running_.load()) break;
            continue;
        }
        nfd.set_nodelay();
        auto cli = std::make_shared<TcpClient>();
        sockaddr_in peer{};
        socklen_t sl = sizeof(peer);
        getpeername(nfd.fd(), reinterpret_cast<sockaddr*>(&peer), &sl);
        cli->peer = peer;
        if (tls_enabled_) {
            if (!cli->tls.accept(tls_ctx_.ctx, nfd.fd())) {
                LOGW("Proxy", "DERP tls accept failed from [%s]", addr_to_str(peer).c_str());
                continue;
            }
            cli->use_tls = true;
        }
        cli->fd = std::move(nfd);
        std::thread(&P2PProxy::tcp_session_loop, this, cli).detach();
    }
}

void P2PProxy::on_avail_query(const sockaddr_in& from) {
    ProxyAvailRsp rsp{};
    {
        std::shared_lock<std::shared_mutex> lk(reg_mu_);
        rsp.available = (uuid2addr_.size() < max_proxy_) ? 1 : 0;
        rsp.used = htons((uint16_t)uuid2addr_.size());
        rsp.max_proxy = htons(max_proxy_);
    }
    send(from, MSG_SP_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
}

// ---------------------------------------------------------------------------
// 定时回收
// ---------------------------------------------------------------------------
void P2PProxy::timer_loop() {
    while (running_) {
        sleep((unsigned)kCountTtl);
        const time_t now = time(nullptr);
        size_t path_count = 0;
        for (auto& shard : path_shards_) {
            std::lock_guard<std::mutex> lk(shard.mu);
            for (auto it = shard.paths.begin(); it != shard.paths.end();) {
                for (auto jt = it->second.begin(); jt != it->second.end();) {
                    if (jt->second.count > 0) jt->second.count--;
                    if (jt->second.count == 0 || now - jt->second.last_active > kRegTtl)
                        jt = it->second.erase(jt);
                    else
                        ++jt;
                }
                if (it->second.empty()) it = shard.paths.erase(it);
                else { path_count += it->second.size(); ++it; }
            }
        }
        std::vector<uint64_t> dead_uuid;
        size_t uuid_n = 0;
        {
            std::unique_lock<std::shared_mutex> lk(reg_mu_);
            for (auto it = uuid2addr_.begin(); it != uuid2addr_.end();) {
                if (now - it->second.last_reg > kRegTtl) {
                    dead_uuid.push_back(addr_to_u64(it->second.addr));
                    it = uuid2addr_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto k : dead_uuid) addr2uuid_.erase(k);
            uuid_n = uuid2addr_.size();
        }
        LOGD("Proxy", "tables: uuid=%zu srcpaths=%zu relay_pkts=%llu quota_drop=%llu",
             uuid_n, path_count, (unsigned long long)relay_pkts_.load(),
             (unsigned long long)relay_drop_quota_.load());
    }
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void P2PProxy::run() {
    running_ = true;
    std::vector<std::thread> recv_threads;
    socks_.clear();
    socks_.reserve((size_t)workers_);
    auto bind_workers = [&](uint16_t p, int n) {
        for (int i = 0; i < n; i++) {
            UdpFd s;
            if (!s.open()) { perror("[Proxy] socket"); continue; }
            if (!s.set_reuse(true) || !s.bind_any(p)) {
                perror("[Proxy] bind");
                continue;   // RAII：失败自动关闭
            }
            socks_.push_back(std::move(s));
        }
    };
    bind_workers(port_, workers_);
    if (alt_port_) {
        const int alt_n = workers_ > 2 ? 2 : workers_;
        bind_workers(alt_port_, alt_n);
        LOGI("Proxy", "TURN-over-443 alt listen %d (%d sockets)", alt_port_, alt_n);
    }
    for (auto& s : socks_)
        recv_threads.emplace_back(&P2PProxy::recv_loop, this, std::cref(s));
    std::thread timer(&P2PProxy::timer_loop, this);
    std::thread tcp_thr;
    if (tcp_port_ && tcp_listen_.valid())
        tcp_thr = std::thread(&P2PProxy::tcp_accept_loop, this);

    LOGI("Proxy", "running with %zu recv sockets tcp=%d", socks_.size(), tcp_port_);
    while (running_) sleep(1);

    running_ = false;
    if (tcp_listen_.valid()) tcp_listen_.shutdown_rw();
    if (tcp_thr.joinable()) tcp_thr.join();
    timer.join();
    // 唤醒阻塞的 recvfrom：向每个 socket 发一个探测包
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto wake = [&](uint16_t p) {
        self.sin_port = htons(p);
        for (size_t i = 0; i < recv_threads.size(); i++)
            socks_[i].send_to("", 0, self);
    };
    wake(port_);
    if (alt_port_) wake(alt_port_);
    for (size_t i = 0; i < recv_threads.size(); i++)
        recv_threads[i].join();
    socks_.clear();   // RAII 统一关闭
    LOGI("Proxy", "stopped, relay_pkts=%llu relay_bytes=%llu",
         (unsigned long long)relay_pkts_.load(),
         (unsigned long long)relay_bytes_.load());
}

} // namespace p2p

// 独立 main：生成单例并启动
int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <Port> <MaxProxyNum> [Workers] [QuotaMB] [AltPort] [TcpPort]\n", argv[0]);
        return 1;
    }
    int workers = argc >= 4 ? atoi(argv[3]) : 4;
    uint64_t quota = 0;
    if (argc >= 5) {
        const long mb = atol(argv[4]);
        if (mb > 0) quota = (uint64_t)mb * 1024ULL * 1024ULL;
    }
    uint16_t alt = 0;
    if (argc >= 6) alt = (uint16_t)atoi(argv[5]);
    else if (const char* e = getenv("P2P_PROXY_ALT_PORT")) alt = (uint16_t)atoi(e);
    uint16_t tcp = 0;
    if (argc >= 7) tcp = (uint16_t)atoi(argv[6]);
    else if (const char* e = getenv("P2P_PROXY_TCP_PORT")) tcp = (uint16_t)atoi(e);
    static p2p::P2PProxy proxy;
    if (proxy.init((uint16_t)atoi(argv[1]), (uint16_t)atoi(argv[2]), workers, quota, alt, tcp) != 0)
        return 1;
    proxy.run();
    return 0;
}
