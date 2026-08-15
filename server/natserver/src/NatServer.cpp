// NatServer 主程序：启动参数仿原实现
//   ./p2p_natserver <NatServerPort> <ProxyServerPort> <WanIP> [P2pServers.cfg]
//   可选环境变量 P2P_STATUS_PORT=NNN 启用 JSON 状态服务
#include "NatServer.h"
#include "Crypto.h"
#include "Log.h"
#include "Packet.h"
#include "Util.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/epoll.h>
#include <unistd.h>

using namespace p2p;

static NatServer* g_srv = nullptr;

static void on_signal(int) { if (g_srv) g_srv->request_stop(); }

NatServer::NatServer() {}
NatServer::~NatServer() {}

int NatServer::init(const std::string& cfg_path, uint16_t nat_port,
                    uint16_t proxy_port, const std::string& wan_ip,
                    uint16_t status_port) {
    nat_port_ = nat_port;
    proxy_port_ = proxy_port;
    status_port_ = status_port;
    wan_ip_ = wan_ip;

    cfg_ = std::make_shared<CfgData>();
    load_server_set(*cfg_, cfg_path);
    if (cfg_->nat_ips.empty() || cfg_->proxy_ips.empty()) {
        LOGE("NatServer", "config empty");
        return -1;
    }
    cfg_mtime_ = cfg_file_mtime(cfg_path);

    abuse_.configure(cfg_->flood_pkt_threshold, cfg_->blacklist_seconds);
    if (!cfg_->blacklist_file.empty()) {
        abuse_.set_path(cfg_->blacklist_file);
        abuse_.set_password(cfg_->blacklist_pass);
        abuse_.load();   // 重启不丢；失败仅告警不影响启动
    }
    if (!cfg_->license_file.empty()) license_.set_path(cfg_->license_file);
    else license_.set_path("/tmp/licensep2p_clean.dat");
    license_.set_password(cfg_->license_pass);
    if (cfg_->enable_license) {
        // 优先从 LicenseFile 加载（含密文），否则用配置内联白名单
        if (!cfg_->license_file.empty() && license_.load(cfg_->license_file)) {
            LOGI("NatServer", "license loaded from %s (%zu uuids)",
                 cfg_->license_file.c_str(), license_.count());
        } else {
            for (const auto& u : cfg_->allowed_uuids) license_.add(u);
        }
        if (!cfg_->license_file.empty() && license_.count() > 0)
            license_.save();   // 首次以密文持久化
    }

    // 双 socket：主端口 + 备用端口（默认临时端口，客户端通过应答获知）
    uint16_t alt_port = cfg_->nat_sock2_port ? cfg_->nat_sock2_port : 0;
    if (natcheck_.init(nat_port_, alt_port) != 0) return -1;

    // 注册表同步：解析对端列表（启动后 run() 中发起首次全量快照）
    setup_sync_peers();

    // 多收包线程：SO_REUSEPORT 克隆 socket（与主 socket 同端口，内核均衡分发）
    recv_socks_.clear();   // RAII：旧 socket 自动关闭
    for (int i = 1; i < cfg_->recv_threads; i++) {
        UdpFd s = make_recv_socket(nat_port_);
        if (!s.valid()) return -1;
        recv_socks_.push_back(std::move(s));
    }

    LOGI("NatServer", "start server with NatServerPort[%d] ProxyServerPort[%d] "
         "NatServerWanIP[%s] AltPort[%d] workers[%d] recv_threads[%zu] sync[%d]",
         nat_port_, proxy_port_, wan_ip_.c_str(), alt_port, cfg_->proc_workers,
         recv_socks_.size() + 1, (int)cfg_->sync_enabled);
    return 0;
}

// 解析同步对端列表：优先 SyncAddrs，否则用配置 NatServer* 列表（排除本机）
void NatServer::setup_sync_peers() {
    sync_addrs_.clear();
    if (!cfg_->sync_enabled) { sync_enabled_ = false; return; }
    sync_enabled_ = true;

    std::vector<std::string> list = cfg_->sync_addrs;
    if (list.empty()) list = cfg_->nat_ips;

    std::string self_ip = wan_ip_;
    if (self_ip == "0.0.0.0") self_ip = "127.0.0.1";
    uint16_t self_port = nat_port_;

    for (auto& s : list) {
        std::string ip = s;
        uint16_t port = nat_port_;
        size_t pos = ip.find(':');
        if (pos != std::string::npos) {
            port = (uint16_t)atoi(ip.c_str() + pos + 1);
            ip = ip.substr(0, pos);
        }
        if (ip == self_ip && port == self_port) continue;  // 跳过本机
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) continue;
        sync_addrs_.push_back(a);
        LOGI("NatServer", "sync peer [%s:%d]", ip.c_str(), port);
    }
    LOGI("NatServer", "registry sync enabled, %zu peer(s)", sync_addrs_.size());
}

// run() 线程组：收包（主 + 克隆）/处理池/定时器/NAT 备用 socket/状态服务
struct NatServer::RunThreads {
    std::thread main_recv;
    std::vector<std::thread> clone_recv;
    std::vector<std::thread> workers;
    std::thread timer;
    std::thread nat_alt;
    std::thread status;
};

namespace {
// 停机唤醒：向本机 TCP 端口发起一次连接，使阻塞在 accept 的线程返回
void wake_tcp_accept(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    connect(fd, (const sockaddr*)&a, sizeof(a));
    close(fd);
}
} // namespace

void NatServer::start_threads(RunThreads& t) {
    t.main_recv = std::thread(&NatServer::recv_thread, this, 0);
    for (size_t i = 0; i < recv_socks_.size(); i++)
        t.clone_recv.emplace_back(&NatServer::recv_thread, this, (int)(i + 1));
    const int workers = cfg_->proc_workers;
    for (int i = 0; i < workers; i++)
        t.workers.emplace_back(&NatServer::proc_pool_thread, this, i);
    t.timer = std::thread(&NatServer::timer_thread, this);
    t.nat_alt = std::thread(&NatTypeCheck::run_alt_thread, &natcheck_);
    if (status_port_ > 0) {
        status_.init(status_port_, [this] { return status_json(); });
        t.status = std::thread(&StatusServer::run, &status_);
    }
    LOGI("NatServer", "start receiving message from client! %zu recv threads + %d workers",
         recv_socks_.size() + 1, workers);
}

void NatServer::stop_threads(RunThreads& t) {
    running_ = false;
    natcheck_.request_stop();
    mq_cv_.notify_all();

    t.main_recv.join();
    for (auto& th : t.clone_recv) th.join();
    for (auto& th : t.workers) th.join();
    t.timer.join();
    t.nat_alt.join();
    recv_socks_.clear();   // RAII 统一关闭
    if (t.status.joinable()) {
        status_.request_stop();
        wake_tcp_accept(status_port_);
        t.status.join();
    }
}

int NatServer::run() {
    g_srv = this;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    running_ = true;
    RunThreads threads;
    start_threads(threads);

    request_sync_snapshot();   // 启动即拉取对端全量注册表
    while (running_) sleep(1);

    stop_threads(threads);

    LOGI("NatServer", "stopped, total_pkts=%llu connect_ok=%llu connect_fail=%llu",
         (unsigned long long)total_pkts_.load(),
         (unsigned long long)connect_ok_.load(),
         (unsigned long long)connect_fail_.load());
    return 0;
}

bool NatServer::send_msg(const sockaddr_in& to, uint8_t msg_id,
                         const void* payload, size_t plen) {
    uint8_t buf[MAX_PKT];
    const size_t n = build_msg(buf, sizeof(buf), msg_id, payload, plen);
    if (n == 0) return false;
    int fd = natcheck_.main_fd();
    return sendto(fd, buf, n, 0, (const sockaddr*)&to, sizeof(to)) > 0;
}

// 鉴权：签发挑战 nonce（30s 有效，一请求一签）
bool NatServer::issue_auth_nonce(const std::string& uuid, uint8_t out_nonce[16]) {
    if (p2p_random_bytes(out_nonce, 16) != 0) {
        LOGE("NatServer", "secure random unavailable, refuse auth nonce");
        return false;
    }
    std::lock_guard<std::mutex> lk(nonce_mu_);
    std::array<uint8_t, 16> n;
    memcpy(n.data(), out_nonce, 16);
    auth_nonces_[uuid] = {n, time(nullptr) + 30};
    return true;
}

// 鉴权：验证 uuid||nonce 的 HMAC（常量时间比较），成功则延长在线期
bool NatServer::verify_auth_login(const std::string& uuid, const uint8_t nonce[16],
                                  const uint8_t mac[32]) {
    auto cfg = cfg_;
    if (!cfg->auth_enabled()) return true;  // 未启用鉴权时放行（兼容模式）

    std::array<uint8_t, 16> n;
    time_t expire = 0;
    {
        std::lock_guard<std::mutex> lk(nonce_mu_);
        auto it = auth_nonces_.find(uuid);
        if (it == auth_nonces_.end()) return false;
        if (time(nullptr) > it->second.second) { auth_nonces_.erase(it); return false; }
        n = it->second.first;
        expire = it->second.second;
        auth_nonces_.erase(it);   // 一次性使用
    }
    if (!p2p_const_time_eq(n.data(), nonce, 16)) return false;

    // expected = HMAC-SHA256(secret, uuid || nonce)
    uint8_t msg[16 + MAX_UUID_LEN + 1];
    memset(msg, 0, sizeof(msg));
    memcpy(msg, uuid.c_str(), uuid.size());
    memcpy(msg + MAX_UUID_LEN + 1, nonce, 16);
    uint8_t expect[32];
    hmac_sha256((const uint8_t*)cfg->auth_secret.data(), cfg->auth_secret.size(),
                msg, 16 + MAX_UUID_LEN + 1, expect);

    if (!p2p_const_time_eq(expect, mac, 32)) return false;

    peers_.set_auth_expire(uuid, (time_t)time(nullptr) + 3600);
    (void)expire;
    return true;
}

// 心跳应答（明文 0x02 / 加密 0x1C）
void NatServer::send_heartbeat_rsp(const sockaddr_in& to, const char* uuid,
                                   const ExtInfoRsp& rsp, bool enc,
                                   const uint8_t enc_iv[8]) {
    if (!enc) {
        send_msg(to, MSG_HEARTBEAT_RSP, &rsp, sizeof(rsp));
        return;
    }
    // 负载 = uuid(33B) || iv(8) || cipher(ExtInfoRsp)
    uint8_t buf[33 + 8 + sizeof(ExtInfoRsp)];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, uuid, 33);
    memcpy(buf + 33, enc_iv, 8);
    memcpy(buf + 41, &rsp, sizeof(rsp));
    auto cfg = cfg_;
    if (!cfg->auth_secret.empty()) {
        p2p_stream_xor((const uint8_t*)cfg->auth_secret.data(), cfg->auth_secret.size(),
                       (const char*)buf, enc_iv, buf + 41, sizeof(rsp));
    }
    send_msg(to, MSG_HEARTBEAT_RSP_ENC, buf, sizeof(buf));
}

// 心跳/注册处理（明文与加密共用）：鉴权/黑名单/白名单门 + 注册 + 应答
void NatServer::handle_heartbeat(const UuidReq& req, const std::string& extinfo,
                                 const sockaddr_in& from, bool enc,
                                 const uint8_t enc_iv[8]) {    auto cfg = cfg_;
    ExtInfoRsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.nat_sock2_port = htons(natcheck_.alt_port());

    // 鉴权/黑名单/白名单门
    if (cfg->auth_enabled() && !peers_.authed(req.uuid)) {
        rsp.result = 1;   // 需鉴权
        send_heartbeat_rsp(from, req.uuid, rsp, enc, enc_iv);
        return;
    }
    if (abuse_.is_uuid_blacklisted(req.uuid) ||
        (cfg->enable_license && !license_.allowed(req.uuid))) {
        rsp.result = 2;   // 黑名单/白名单拒绝
        send_heartbeat_rsp(from, req.uuid, rsp, enc, enc_iv);
        return;
    }

    sockaddr_in lan = from;
    lan.sin_port = req.lan_port;
    peers_.upsert(req.uuid, from, lan, req.dev_type, req.nattype, extinfo);

    // 注册表同步：向其它 NatServer 广播本机节点增/更
    broadcast_peer_entry(req, extinfo, from);

    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
    inet_ntop(AF_INET, &from.sin_addr, rsp.pub_ip, MAX_IP_LEN);
    rsp.pub_port = from.sin_port;
    inet_ntop(AF_INET, &lan.sin_addr, rsp.lan_ip, MAX_IP_LEN);
    rsp.lan_port = lan.sin_port;
    rsp.result = 0;
    send_heartbeat_rsp(from, req.uuid, rsp, enc, enc_iv);

    LOGI("NatServer", "heartbeat/register UUID=[%s] type=[%d] nattype=[%d] "
         "pub=[%s:%d] lan_port=[%d] ext=%zu enc=%d",
         req.uuid, req.dev_type, req.nattype, ipbuf, ntohs(from.sin_port),
         ntohs(lan.sin_port), extinfo.size(), (int)enc);
}

// epoll 收包线程（idx=0 主 socket；idx>=1 为 SO_REUSEPORT 克隆 socket）：
// NAT 探测走快速路径，其余入有界队列
UdpFd NatServer::make_recv_socket(uint16_t port) {
    UdpFd s;
    if (!s.open()) { perror("[NatServer] socket"); return s; }
    if (!s.set_reuse(true) || !s.bind_any(port)) {
        perror("[NatServer] recv socket bind");
        s.close();
        return s;
    }
    LOGI("NatServer", "recv clone socket fd=%d port=%d", s.fd(), port);
    return s;
}

void NatServer::recv_thread(int idx) {
    const int fd = (idx == 0) ? natcheck_.main_fd() : recv_socks_[idx - 1].fd();
    EpollFd ep;   // RAII：任何退出路径自动关闭
    if (!ep.create()) { perror("[NatServer] epoll_create1"); running_ = false; return; }
    if (!ep.add_read(fd)) {
        perror("[NatServer] epoll_ctl");
        running_ = false;
        return;
    }

    epoll_event events[16];
    while (running_) {
        int n = ep.wait(events, 16, 1000);
        if (n < 0 && errno != EINTR) perror("[NatServer] epoll_wait");
        for (int i = 0; i < n; i++) {
            if (!(events[i].events & EPOLLIN)) continue;
            for (;;) {
                uint8_t buf[MAX_PKT];
                sockaddr_in from;
                socklen_t from_len = sizeof(from);
                ssize_t r = recvfrom(fd, buf, sizeof(buf), 0,
                                     (sockaddr*)&from, &from_len);
                if (r <= 0) break;

                total_pkts_.fetch_add(1);
                if (abuse_.is_ip_blacklisted(from)) continue;
                if (abuse_.check_flood(from)) continue;

                // 快速路径：NAT 类型探测（双 socket 应答，不进队列）
                if (natcheck_.try_fast_handle(buf, (size_t)r, from)) continue;

                IncomingPacket pkt;
                pkt.len = (size_t)r;
                pkt.from = from;
                memcpy(pkt.buf, buf, pkt.len);

                std::lock_guard<std::mutex> lk(mq_mu_);
                if (mq_.size() >= 8192) mq_.pop_front();
                mq_.push_back(std::move(pkt));
                mq_cv_.notify_one();
            }
        }
    }
}

// 报文处理线程池
void NatServer::proc_pool_thread(int id) {
    (void)id;
    while (running_) {
        IncomingPacket pkt;
        {
            std::unique_lock<std::mutex> lk(mq_mu_);
            mq_cv_.wait_for(lk, std::chrono::milliseconds(200),
                            [&] { return !mq_.empty() || !running_; });
            if (mq_.empty()) continue;
            pkt = std::move(mq_.front());
            mq_.pop_front();
        }
        handle_packet(pkt.buf, pkt.len, pkt.from);
    }
}

// 定时任务线程
void NatServer::timer_thread() {
    time_t last_proxy_q = 0;
    time_t last_stat = 0;
    time_t last_sync_snap = 0;
    while (running_) {
        time_t now = time(nullptr);

        // 心跳超时离线：清理 + 向对端广播删除
        auto timed_out = peers_.cleanup_timeout_and_collect(90);
        for (auto& u : timed_out) {
            SyncPeerDel d;
            memset(&d, 0, sizeof(d));
            strncpy(d.uuid, u.c_str(), MAX_UUID_LEN);
            broadcast_sync_msg(MSG_SYNC_PEER_DEL, &d, sizeof(d), nullptr);
        }
        peers_.cleanup_auth_sessions();   // 清理过期鉴权会话
        abuse_.sweep();
        if (abuse_.needs_save() && abuse_.save()) abuse_.reset_dirty();  // 黑名单持久化

        // 配置热加载：先解析到新对象，再原子替换，避免并发读脏数据
        auto cur = std::make_shared<CfgData>(*cfg_);
        if (reload_if_changed(*cur, cfg_->cfg_path, &cfg_mtime_)) {
            cfg_ = cur;
            abuse_.configure(cur->flood_pkt_threshold, cur->blacklist_seconds);
            LOGI("NatServer", "config reloaded");
        }

        if (now - last_proxy_q >= 10) {   // 每 10s 探测代理可用性
            last_proxy_q = now;
            for (auto& ip : cfg_->proxy_ips) send_proxy_avail_query(ip);
            mark_proxy_stale();           // 未应答者 down++，用于失败转移加权
        }
        if (sync_enabled() && now - last_sync_snap >= SYNC_SNAPSHOT_INTERVAL) {
            last_sync_snap = now;
            request_sync_snapshot();      // 周期全量快照，兜底增量丢失
        }
        if (now - last_stat >= 5) {       // 每 5s 打印统计
            last_stat = now;
            printf("numPeers:%zu (dev:%zu app:%zu authed:%zu) total_pkts:%llu "
                   "connect_ok:%llu fail:%llu black_ip:%zu\n",
                   peers_.size(), peers_.device_count(), peers_.client_count(),
                   peers_.authed_count(), (unsigned long long)total_pkts_.load(),
                   (unsigned long long)connect_ok_.load(),
                   (unsigned long long)connect_fail_.load(),
                   abuse_.ip_blacklist_size());
            fflush(stdout);
        }
        sleep(1);
    }
}

void NatServer::send_proxy_avail_query(const std::string& proxy_ip) {
    ProxyAvailReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.nat_ip, wan_ip_.c_str(), MAX_IP_LEN - 1);
    req.nat_port = htons(nat_port_);

    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(proxy_port_);
    inet_pton(AF_INET, proxy_ip.c_str(), &to.sin_addr);
    send_msg(to, MSG_SP_ASK_EXTINFO_REQ, &req, sizeof(req));
}

void NatServer::collect_proxy_avail(const sockaddr_in& from, const ProxyAvailRsp& rsp) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    std::lock_guard<std::mutex> lk(proxy_mu_);
    for (auto& p : proxy_health_) {
        if (p.ip == ip && p.port == proxy_port_) {
            p.available = rsp.available;
            p.used = ntohs(rsp.used);
            p.max_proxy = ntohs(rsp.max_proxy);
            p.last_seen = time(nullptr);
            p.down = 0;
            return;
        }
    }
    ProxyHealth h;
    h.ip = ip;
    h.port = proxy_port_;
    h.available = rsp.available;
    h.used = ntohs(rsp.used);
    h.max_proxy = ntohs(rsp.max_proxy);
    h.last_seen = time(nullptr);
    h.down = 0;
    proxy_health_.push_back(h);
}

// 周期探测未收到应答的代理 down++（封顶 255），超过阈值降权/剔除
void NatServer::mark_proxy_stale() {
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(proxy_mu_);
    for (auto& p : proxy_health_) {
        if (now - p.last_seen >= 10) {
            if (p.down < 255) p.down++;
            p.available = 0;
        }
    }
}

// 代理择优调度：健康代理按空闲度加权随机（分散首选，天然失败转移）；
// 无健康数据时回退配置顺序。始终给出至多 3 个候选。
void NatServer::pick_proxy(ProxyCandidate out[3], uint8_t& count) {
    count = 0;
    struct Cand {
        std::string ip;
        double weight;
    };
    std::vector<Cand> cands;
    {
        std::lock_guard<std::mutex> lk(proxy_mu_);
        time_t now = time(nullptr);
        for (auto& p : proxy_health_) {
            bool fresh = now - p.last_seen <= 30;
            bool healthy = fresh && p.available && p.down < 2;
            if (!healthy) continue;
            double util = p.max_proxy ? (double)p.used / p.max_proxy : 1.0;
            double w = 1.0 - util;
            if (w < 0.05) w = 0.05;
            w *= 1.0 - 0.3 * p.down;   // 连续失联扣分
            if (w < 0.02) w = 0.02;
            cands.push_back({p.ip, w});
        }
    }

    // 加权随机挑选（不打乱去重顺序，保证候选互不重复）
    auto pick_weighted = [&]() -> std::string {
        double total = 0;
        for (auto& c : cands) total += c.weight;
        if (total <= 0) return "";
        double r = (double)(rand_u32() % 10000) / 10000.0 * total;
        for (auto& c : cands) {
            if (r < c.weight) return c.ip;
            r -= c.weight;
        }
        return cands.back().ip;
    };
    auto dup = [&](const std::string& s) {
        for (uint8_t i = 0; i < count; i++)
            if (s == out[i].ip) return true;
        return false;
    };

    for (int k = 0; k < 3 && !cands.empty(); k++) {
        std::string ip = pick_weighted();
        if (!ip.empty() && !dup(ip)) {
            memset(out + count, 0, sizeof(ProxyCandidate));
            strncpy(out[count].ip, ip.c_str(), MAX_IP_LEN - 1);
            out[count].port = htons(proxy_port_);
            count++;
        }
    }

    // 候选不足时按配置顺序回退（保证永远给足 3 个入口）
    for (auto& ip : cfg_->proxy_ips) {
        if (count >= 3) break;
        if (dup(ip)) continue;
        memset(out + count, 0, sizeof(ProxyCandidate));
        strncpy(out[count].ip, ip.c_str(), MAX_IP_LEN - 1);
        out[count].port = htons(proxy_port_);
        count++;
    }
}

// 负载末尾附加 HMAC-SHA256(SyncAuthSecret, payload) 后发送（未配置密钥则原样发送）
void NatServer::sync_send_msg(const sockaddr_in& to, uint8_t msg_id,
                              const uint8_t* payload, size_t plen) {
    auto cfg = cfg_;
    uint8_t buf[MAX_PKT];
    if (plen > sizeof(buf) - 32) return;
    memcpy(buf, payload, plen);
    if (!cfg->sync_auth_secret.empty()) {
        uint8_t mac[32];
        hmac_sha256((const uint8_t*)cfg->sync_auth_secret.data(),
                    cfg->sync_auth_secret.size(), payload, plen, mac);
        memcpy(buf + plen, mac, 32);
        plen += 32;
    }
    send_msg(to, msg_id, buf, plen);
}

// 校验同步消息末尾 HMAC；未配置 SyncAuthSecret 视为放行（兼容旧版）
bool NatServer::sync_verify(const uint8_t* payload, size_t plen,
                            size_t body_len) const {
    auto cfg = cfg_;
    if (cfg->sync_auth_secret.empty()) return true;
    if (body_len + 32 > plen) return false;
    uint8_t expect[32];
    hmac_sha256((const uint8_t*)cfg->sync_auth_secret.data(),
                cfg->sync_auth_secret.size(), payload, body_len, expect);
    return p2p_const_time_eq(expect, payload + body_len, 32);
}

// 向所有同步对端广播一条消息（except 可选排除来源）
void NatServer::broadcast_sync_msg(uint8_t msg_id, const void* payload, size_t plen,
                                   const sockaddr_in* except) {
    for (auto& to : sync_addrs_) {
        if (except && except->sin_addr.s_addr == to.sin_addr.s_addr &&
            except->sin_port == to.sin_port)
            continue;
        sync_send_msg(to, msg_id, (const uint8_t*)payload, plen);
    }
}

// 防环去重：已见过相同 (uuid, hb_time, src) 的条目返回 true
bool NatServer::seen_sync_entry(const std::string& uuid, uint32_t hb_time,
                                const sockaddr_in& from) {
    char key[160];
    snprintf(key, sizeof(key), "%s|%u|%u:%u", uuid.c_str(), hb_time,
             ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(sync_mu_);
    auto it = sync_seen_.find(key);
    if (it != sync_seen_.end() && now - it->second < 300) return true;
    sync_seen_[key] = now;
    if (sync_seen_.size() > 16384) {
        for (auto jt = sync_seen_.begin(); jt != sync_seen_.end();) {
            if (now - jt->second >= 300) jt = sync_seen_.erase(jt);
            else ++jt;
        }
    }
    return false;
}

// 向对端 to 发送一条对等节点记录
void NatServer::send_peer_entry(const sockaddr_in& to, const Peer& p, uint8_t hop) {
    uint8_t buf[sizeof(SyncPeerEntry) + MAX_EXTINFO];
    memset(buf, 0, sizeof(buf));
    SyncPeerEntry* e = reinterpret_cast<SyncPeerEntry*>(buf);
    strncpy(e->uuid, p.uuid.c_str(), MAX_UUID_LEN);
    inet_ntop(AF_INET, &p.pub_addr.sin_addr, e->pub_ip, MAX_IP_LEN);
    e->pub_port = p.pub_addr.sin_port;
    inet_ntop(AF_INET, &p.lan_addr.sin_addr, e->lan_ip, MAX_IP_LEN);
    e->lan_port = p.lan_addr.sin_port;
    e->dev_type = p.dev_type;
    e->nattype = p.nattype;
    e->hop = hop;
    e->hb_time = htonl((uint32_t)p.last_heartbeat);
    e->extlen = htons((uint16_t)p.extlen);
    size_t total = sizeof(SyncPeerEntry);
    if (p.extlen > 0 && p.extlen <= MAX_EXTINFO) {
        memcpy(buf + total, p.extinfo.data(), p.extlen);
        total += p.extlen;
    }
    sync_send_msg(to, MSG_SYNC_PEER_ENTRY, buf, total);
}

// 广播一条注册记录（增量：hop=1 开始）
void NatServer::broadcast_peer_entry(const UuidReq& req, const std::string& extinfo,
                                     const sockaddr_in& pub) {
    if (!sync_enabled()) return;
    Peer p;
    p.uuid = req.uuid;
    p.pub_addr = pub;
    p.lan_addr = pub;
    p.lan_addr.sin_port = req.lan_port;
    p.dev_type = req.dev_type;
    p.nattype = req.nattype;
    p.last_heartbeat = time(nullptr);
    p.extinfo = extinfo;
    p.extlen = (uint16_t)extinfo.size();
    send_peer_entry(sync_addrs_[0], p, 1);
    for (size_t i = 1; i < sync_addrs_.size(); i++)
        send_peer_entry(sync_addrs_[i], p, 1);
}

// 收到对端广播/快照的对等节点记录：去重 -> 落地 -> 转发（防环）
void NatServer::handle_sync_entry(const SyncPeerEntry& e, const std::string& extinfo,
                                  const sockaddr_in& from) {
    if (!sync_enabled()) return;

    uint8_t hop = e.hop;
    uint32_t hb_time = ntohl(e.hb_time);
    std::string uuid(e.uuid);
    if (uuid.empty()) return;

    if (hop != SYNC_HOP_SNAP && seen_sync_entry(uuid, hb_time, from)) {
        LOGI("NatServer", "sync entry dup skip uuid[%s] hb[%u] from [%s:%d]",
             uuid.c_str(), hb_time, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        return;
    }

    sockaddr_in pub, lan;
    memset(&pub, 0, sizeof(pub));
    pub.sin_family = AF_INET;
    pub.sin_port = e.pub_port;
    inet_pton(AF_INET, e.pub_ip, &pub.sin_addr);
    lan = pub;
    lan.sin_port = e.lan_port;
    inet_pton(AF_INET, e.lan_ip, &lan.sin_addr);
    peers_.upsert_synced(uuid, pub, lan, e.dev_type, e.nattype, extinfo, (time_t)hb_time);

    LOGI("NatServer", "sync entry uuid[%s] hop[%d] hb[%u] pub[%s:%d] from [%s:%d]",
         uuid.c_str(), hop, hb_time, e.pub_ip, ntohs(e.pub_port),
         inet_ntoa(from.sin_addr), ntohs(from.sin_port));

    // 增量条目逐跳转发（快照条目不转发）
    if (hop != SYNC_HOP_SNAP && hop < SYNC_HOP_MAX) {
        SyncPeerEntry f = e;
        f.hop = hop + 1;
        broadcast_sync_msg(MSG_SYNC_PEER_ENTRY, &f, sizeof(f) + extinfo.size(),
                           &from);
    }
}

// 收到对端删除通知：转发并本地删除
void NatServer::handle_sync_del(const std::string& uuid, const sockaddr_in& from) {
    if (!sync_enabled()) return;
    SyncPeerDel d;
    memset(&d, 0, sizeof(d));
    strncpy(d.uuid, uuid.c_str(), MAX_UUID_LEN);
    broadcast_sync_msg(MSG_SYNC_PEER_DEL, &d, sizeof(d), &from);
    peers_.remove(uuid);
    LOGI("NatServer", "sync del uuid[%s] from [%s:%d]",
         uuid.c_str(), inet_ntoa(from.sin_addr), ntohs(from.sin_port));
}

// 向对端 to 全量快照本机注册表（快照条目不转发）
void NatServer::send_sync_snapshot(const sockaddr_in& to) {
    auto snap = peers_.snapshot();
    for (auto& p : snap) send_peer_entry(to, p, SYNC_HOP_SNAP);
    LOGI("NatServer", "sync snapshot sent %zu entries to [%s:%d]", snap.size(),
         inet_ntoa(to.sin_addr), ntohs(to.sin_port));
}

// 收到全量快照请求：以多条 MSG_SYNC_PEER_ENTRY 应答
void NatServer::handle_sync_snapshot_req(const sockaddr_in& from) {
    if (!sync_enabled()) return;
    send_sync_snapshot(from);
}

// 向所有同步对端请求全量快照（带时间戳防重放；广播逻辑会附加尾部 HMAC）
void NatServer::request_sync_snapshot() {
    if (!sync_enabled()) return;
    SyncSnapshotReq req;
    req.ts = htonl((uint32_t)time(nullptr));
    broadcast_sync_msg(MSG_SYNC_SNAPSHOT_REQ, &req, sizeof(req), nullptr);
}

std::string NatServer::status_json() const {
    std::ostringstream os;
    os << "{"
       << "\"online\":" << peers_.size()
       << ",\"devices\":" << peers_.device_count()
       << ",\"apps\":" << peers_.client_count()
       << ",\"authed\":" << peers_.authed_count()
       << ",\"total_pkts\":" << total_pkts_.load()
       << ",\"connect_ok\":" << connect_ok_.load()
       << ",\"connect_fail\":" << connect_fail_.load()
       << ",\"blacklist_ips\":" << abuse_.ip_blacklist_size()
       << ",\"proxy_count\":" << cfg_->proxy_ips.size()
       << "}";
    return os.str();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s <NatServerPort> <ProxyServerPort> <WanIP> [P2pServers.cfg]\n"
               "  env P2P_STATUS_PORT=NNN  启用 JSON 状态服务\n", argv[0]);
        return 1;
    }
    uint16_t nat_port = (uint16_t)atoi(argv[1]);
    uint16_t proxy_port = (uint16_t)atoi(argv[2]);
    std::string wan_ip = argv[3];
    std::string cfg_path = "P2pServers.cfg";
    if (argc >= 5) cfg_path = argv[4];

    uint16_t status_port = 0;
    if (const char* sp = getenv("P2P_STATUS_PORT")) status_port = (uint16_t)atoi(sp);

    p2p::NatServer srv;
    if (srv.init(cfg_path, nat_port, proxy_port, wan_ip, status_port) != 0) return 1;
    return srv.run();
}
