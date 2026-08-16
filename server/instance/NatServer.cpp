// NatServer 主程序：启动参数仿原实现
//   ./p2p_natserver <NatServerPort> <ProxyServerPort> <WanIP> [P2pServers.cfg]
//   可选：P2pServers.cfg 的 StatusPort / P2P_STATUS_PORT 启用 JSON 状态服务
//   覆盖顺序：文件 < P2P_* 环境变量 < 本函数 status_port 参数
#include "server/instance/NatServer.h"
#include "core/foundation/Crypto.h"
#include "core/foundation/Log.h"
#include "core/socket/Packet.h"
#include "core/foundation/ConnectToken.h"
#include "core/foundation/RegionSched.h"
#include "core/packet/StunBind.h"
#include "core/foundation/Uid.h"
#include "core/foundation/Util.h"
#include "server/listener/UdpListen.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/epoll.h>
#include <sys/select.h>
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
    wan_ip_ = wan_ip;

    cfg_ = std::make_shared<CfgData>();
    load_server_set(*cfg_, cfg_path);
    if (cfg_->private_mode && !cfg_->auth_enabled())
        LOGW("NatServer", "PrivateMode=1 but AuthSecret empty; auth still off");
    sync_.bind([this](const sockaddr_in& to, uint8_t id, const void* p, size_t n) {
        return send_msg(to, id, p, n);
    }, &peers_);
    sync_.set_cfg(cfg_);
    status_port_ = status_port > 0 ? status_port : cfg_->status_port;
    if (cfg_->nat_ips.empty() || cfg_->proxy_ips.empty()) {
        LOGE("NatServer", "config empty");
        return -1;
    }
    cfg_mtime_ = cfg_file_mtime(cfg_path);
    started_at_ = time(nullptr);

    abuse_.configure(cfg_->flood_pkt_threshold, cfg_->blacklist_seconds);
    if (!cfg_->blacklist_file.empty()) {
        abuse_.set_path(cfg_->blacklist_file);
        abuse_.set_password(cfg_->blacklist_pass);
        abuse_.load();   // 重启不丢；失败仅告警不影响启动
    }
    if (!cfg_->jail_file.empty()) {
        abuse_.set_jail_path(cfg_->jail_file);
        abuse_.dump_jail();
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

    // 三 socket：主 + 备用（过滤/映射）+ 第三探测口（NAT4E；默认临时端口）
    uint16_t alt_port = cfg_->nat_sock2_port ? cfg_->nat_sock2_port : 0;
    uint16_t probe_port = cfg_->nat_sock3_port ? cfg_->nat_sock3_port : 0;
    if (natcheck_.init(nat_port_, alt_port, probe_port) != 0) return -1;
    natcheck_.set_advertise(wan_ip_.c_str());

    listeners_.clear();
    listeners_.add_any(nat_port_);
    listeners_.add_ip(wan_ip_.c_str(), nat_port_);
    if (wan_ip_ == "0.0.0.0" || wan_ip_ == "127.0.0.1")
        listeners_.add_ip("127.0.0.1", nat_port_);

    // 注册表同步：解析对端列表（启动后 run() 中发起首次全量快照）
    setup_sync_peers();

    // 多收包线程：SO_REUSEPORT 克隆 socket（与主 socket 同端口，内核均衡分发）
    recv_socks_.clear();   // RAII：旧 socket 自动关闭
    for (int i = 1; i < cfg_->recv_threads; i++) {
        UdpFd s = make_recv_socket(nat_port_);
        if (!s.valid()) return -1;
        recv_socks_.push_back(std::move(s));
    }

    if (stun_tcp_.open() && stun_tcp_.set_reuse() && stun_tcp_.bind_any(nat_port_) &&
        stun_tcp_.listen(16)) {
        LOGI("NatServer", "TCP STUN listen %d (tcp punch mapping)", nat_port_);
    } else {
        stun_tcp_.close();
        LOGW("NatServer", "TCP STUN listen %d failed (tcp punch falls back)", nat_port_);
    }

    LOGI("NatServer", "start server with NatServerPort[%d] ProxyServerPort[%d] "
         "NatServerWanIP[%s] AltPort[%d] ProbePort[%d] workers[%d] recv_threads[%zu] sync[%d]",
         nat_port_, proxy_port_, wan_ip_.c_str(), alt_port, natcheck_.probe_port(),
         cfg_->proc_workers,
         recv_socks_.size() + 1, (int)cfg_->sync_enabled);
    return 0;
}

void NatServer::setup_sync_peers() {
    sync_.set_cfg(cfg_);
    sync_.setup(wan_ip_, nat_port_, listeners_);
}

// run() 线程组：收包（主 + 克隆）/处理池/定时器/NAT 备用 socket/状态服务
struct NatServer::RunThreads {
    std::thread main_recv;
    std::vector<std::thread> clone_recv;
    std::vector<std::thread> workers;
    std::thread timer;
    std::thread nat_alt;
    std::thread status;
    std::thread stun_tcp;
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
        status_.init(status_port_,
                     [this] { return status_json(); },
                     [this] { return status_metrics(); },
                     cfg_->status_allow);
        t.status = std::thread(&StatusServer::run, &status_);
    }
    if (stun_tcp_.valid())
        t.stun_tcp = std::thread(&NatServer::stun_tcp_thread, this);
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
    if (t.stun_tcp.joinable()) {
        wake_tcp_accept(nat_port_);
        t.stun_tcp.join();
    }
    stun_tcp_.close();
}

void NatServer::stun_tcp_thread() {
    if (!stun_tcp_.valid()) return;
    stun_tcp_.set_nonblock();
    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(stun_tcp_.fd(), &rfds);
        timeval tv{};
        tv.tv_usec = 200000;
        const int r = select(stun_tcp_.fd() + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        TcpFd c = stun_tcp_.accept_one();
        if (!c.valid()) continue;
        c.set_timeout_ms(800);
        uint8_t req[STUN_HDR_LEN];
        const ssize_t n = c.recv_some(req, sizeof(req));
        if (n < (ssize_t)STUN_HDR_LEN || !stun_is_binding_request(req, (size_t)n))
            continue;
        sockaddr_in peer{};
        socklen_t sl = sizeof(peer);
        if (getpeername(c.fd(), reinterpret_cast<sockaddr*>(&peer), &sl) != 0)
            continue;
        uint8_t rsp[STUN_BINDING_SUCCESS_LEN];
        if (stun_write_binding_success(rsp, sizeof(rsp), req, (size_t)n, peer) == 0)
            continue;
        c.send_all(rsp, STUN_BINDING_SUCCESS_LEN);
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

bool NatServer::issue_auth_nonce(const std::string& uuid, uint8_t out_nonce[16]) {
    if (!auth_.issue(uuid, out_nonce)) {
        LOGE("NatServer", "secure random unavailable, refuse auth nonce");
        return false;
    }
    return true;
}

bool NatServer::verify_auth_login(const std::string& uuid, const uint8_t nonce[16],
                                  const uint8_t mac[32]) {
    auto cfg = cfg_;
    if (!auth_.verify(*cfg, uuid, nonce, mac)) return false;
    if (cfg->auth_enabled())
        peers_.set_auth_expire(uuid, (time_t)time(nullptr) + 3600);
    return true;
}

bool NatServer::verify_connect_token(const char* src_uuid, const char* dst_uuid,
                                     const uint8_t* trailer, size_t tlen) {
    auto cfg = cfg_;
    if (!cfg) return false;
    return verify_connect_trailer(*cfg, token_nonces_, src_uuid, dst_uuid,
                                  trailer, tlen);
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
        // P1：流密钥用每 UID 派生密钥，设备间无法互解心跳
        uint8_t uid_key[AUTH_KEY_LEN];
        uid_derive_auth_key((const uint8_t*)cfg->auth_secret.data(),
                            cfg->auth_secret.size(), uuid, uid_key);
        p2p_stream_xor(uid_key, AUTH_KEY_LEN,
                       (const char*)buf, enc_iv, buf + 41, sizeof(rsp));
    }
    send_msg(to, MSG_HEARTBEAT_RSP_ENC, buf, sizeof(buf));
}

// 心跳/注册处理（明文与加密共用）：鉴权/黑名单/白名单门 + 注册 + 应答
void NatServer::handle_heartbeat(const UuidReq& req, const std::string& extinfo,
                                 const sockaddr_in& from, bool enc,
                                 const uint8_t enc_iv[8]) {
    auto cfg = cfg_;
    ExtInfoRsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.nat_sock2_port = htons(natcheck_.alt_port());

    // P1：严格 UID 模式下拒绝非结构化 UID（格式/CRC 校验）
    if (cfg->uid_strict && !uid_valid(req.uuid)) {
        rsp.result = 2;
        send_heartbeat_rsp(from, req.uuid, rsp, enc, enc_iv);
        LOGW("NatServer", "heartbeat rejected, invalid UID=[%s]", req.uuid);
        return;
    }
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
    UdpFd s = open_reuseport_udp(port);
    if (!s.valid()) {
        perror("[NatServer] recv socket bind");
        return s;
    }
    LOGI("NatServer", "recv clone socket fd=%d port=%d", s.fd(), port);
    return s;
}

void NatServer::note_punch_admit(PunchAdmit a) {
    switch (a) {
    case PunchAdmit::Ice:     punch_ice_.fetch_add(1); break;
    case PunchAdmit::Relay:   punch_relay_.fetch_add(1); break;
    case PunchAdmit::Unknown: punch_unknown_.fetch_add(1); break;
    }
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

                // 快速路径：RFC 8489/5780 STUN Binding（CHANGE-REQUEST 走备用口）
                if (stun_is_binding_request(buf, (size_t)r)) {
                    natcheck_.handle_stun(fd, buf, (size_t)r, from, false);
                    continue;
                }
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
        natcheck_.cleanup_observe(now);
        abuse_.sweep();
        if (abuse_.needs_save()) {
            if (abuse_.save()) abuse_.reset_dirty();
            abuse_.dump_jail();
        }

        // 配置热加载：先解析到新对象，再原子替换，避免并发读脏数据
        auto cur = std::make_shared<CfgData>(*cfg_);
        if (reload_if_changed(*cur, cfg_->cfg_path, &cfg_mtime_)) {
            cfg_ = cur;
            sync_.set_cfg(cfg_);
            abuse_.configure(cur->flood_pkt_threshold, cur->blacklist_seconds);
            if (!cur->blacklist_file.empty()) {
                abuse_.set_path(cur->blacklist_file);
                abuse_.set_password(cur->blacklist_pass);
            }
            abuse_.set_jail_path(cur->jail_file);
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
    relays_.collect(from, proxy_port_, rsp);
}

void NatServer::mark_proxy_stale() {
    relays_.mark_stale();
}

void NatServer::pick_proxy(ProxyCandidate out[3], uint8_t& count, char prefer_region) {
    auto cfg = cfg_;
    if (!cfg) { count = 0; return; }
    relays_.pick(out, count, *cfg, proxy_port_, prefer_region);
}

void NatServer::sync_send_msg(const sockaddr_in& to, uint8_t msg_id,
                              const uint8_t* payload, size_t plen) {
    sync_.send_signed(to, msg_id, payload, plen);
}

bool NatServer::sync_verify(const uint8_t* payload, size_t plen,
                            size_t body_len) const {
    return sync_.verify(payload, plen, body_len);
}

void NatServer::broadcast_sync_msg(uint8_t msg_id, const void* payload, size_t plen,
                                   const sockaddr_in* except) {
    sync_.broadcast(msg_id, payload, plen, except);
}

void NatServer::broadcast_peer_entry(const UuidReq& req, const std::string& extinfo,
                                     const sockaddr_in& pub) {
    sync_.broadcast_peer(req, extinfo, pub);
}

void NatServer::handle_sync_entry(const SyncPeerEntry& e, const std::string& extinfo,
                                  const sockaddr_in& from) {
    sync_.handle_entry(e, extinfo, from);
}

void NatServer::handle_sync_del(const std::string& uuid, const sockaddr_in& from) {
    sync_.handle_del(uuid, from);
}

void NatServer::handle_sync_snapshot_req(const sockaddr_in& from) {
    sync_.handle_snapshot_req(from);
}

void NatServer::request_sync_snapshot() {
    sync_.request_snapshot();
}

void NatServer::notify_wake(const char* uuid) {
    if (!uuid || !uuid[0] || !cfg_) return;
    const std::string& spec = cfg_->wake_server;
    if (spec.empty()) return;
    std::string ip = spec;
    uint16_t port = 0;
    size_t pos = ip.rfind(':');
    if (pos == std::string::npos) return;
    port = (uint16_t)atoi(ip.c_str() + pos + 1);
    ip = ip.substr(0, pos);
    if (ip.empty() || port == 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return;
    WakeTrigger t{};
    copy_str_field(t.uuid, sizeof(t.uuid), uuid);
    send_msg(to, MSG_WAKE_TRIGGER, &t, sizeof(t));
    LOGI("NatServer", "wake trigger uuid[%s] -> %s:%u", uuid, ip.c_str(), (unsigned)port);
}

static std::string json_safe_name(const std::string& s) {
    std::string o;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            o += c;
        if (o.size() >= 64) break;
    }
    return o;
}

std::string NatServer::status_json() const {
    auto cfg = cfg_;
    const char region = cfg ? cfg->region : 0;
    const std::string inst = json_safe_name(cfg ? cfg->instance_name : "");
    std::ostringstream os;
    os << "{"
       << "\"instance\":\"" << inst << "\""
       << ",\"online\":" << peers_.size()
       << ",\"devices\":" << peers_.device_count()
       << ",\"apps\":" << peers_.client_count()
       << ",\"authed\":" << peers_.authed_count()
       << ",\"total_pkts\":" << total_pkts_.load()
       << ",\"connect_ok\":" << connect_ok_.load()
       << ",\"connect_fail\":" << connect_fail_.load()
       << ",\"login_ok\":" << login_ok_.load()
       << ",\"login_fail\":" << login_fail_.load()
       << ",\"punch_ice\":" << punch_ice_.load()
       << ",\"punch_relay\":" << punch_relay_.load()
       << ",\"punch_unknown\":" << punch_unknown_.load()
       << ",\"stun_same\":" << natcheck_.stun_same()
       << ",\"stun_other\":" << natcheck_.stun_other()
       << ",\"blacklist_ips\":" << abuse_.ip_blacklist_size()
       << ",\"flood_drop\":" << abuse_.dropped_flood_pkts()
       << ",\"need_p2p\":" << peers_.need_p2p_count()
       << ",\"private_mode\":" << (cfg && cfg->private_mode ? "true" : "false")
       << ",\"sync\":" << (sync_.enabled() ? "true" : "false")
       << ",\"relay_reject\":" << relay_reject_.load()
       << ",\"proxy_count\":" << (cfg ? cfg->proxy_ips.size() : 0)
       << ",\"region\":\"" << (region ? std::string(1, region) : "") << "\""
       << ",\"uptime_seconds\":" << (started_at_ ? (long)(time(nullptr) - started_at_) : 0)
       << "}";
    return os.str();
}

std::string NatServer::status_metrics() const {
    auto cfg = cfg_;
    const char region = cfg ? cfg->region : 0;
    const long uptime = started_at_ ? (long)(time(nullptr) - started_at_) : 0;
    std::ostringstream os;
    auto gauge = [&](const char* name, const char* help, long v) {
        os << "# HELP " << name << " " << help << "\n"
           << "# TYPE " << name << " gauge\n"
           << name << " " << v << "\n";
    };
    auto counter = [&](const char* name, const char* help, unsigned long long v) {
        os << "# HELP " << name << " " << help << "\n"
           << "# TYPE " << name << " counter\n"
           << name << " " << v << "\n";
    };
    gauge("p2p_online_peers", "Online peers in the registry", (long)peers_.size());
    gauge("p2p_online_devices", "Online device-role peers", (long)peers_.device_count());
    gauge("p2p_online_apps", "Online app-role peers", (long)peers_.client_count());
    gauge("p2p_authed_peers", "Authenticated peers", (long)peers_.authed_count());
    gauge("p2p_blacklist_ips", "Blacklisted source IPs", (long)abuse_.ip_blacklist_size());
    gauge("p2p_need_p2p_peers", "Peers advertising need_p2p (np=1)",
          (long)peers_.need_p2p_count());
    gauge("p2p_private_mode", "PrivateMode (1=on)",
          (long)(cfg && cfg->private_mode ? 1 : 0));
    gauge("p2p_registry_sync", "RegistrySync enabled (1=on)",
          (long)(sync_.enabled() ? 1 : 0));
    gauge("p2p_proxy_configured", "Configured proxy endpoints",
          (long)(cfg ? cfg->proxy_ips.size() : 0));
    gauge("p2p_uptime_seconds", "Process uptime in seconds", uptime);
    counter("p2p_packets_total", "UDP packets processed", total_pkts_.load());
    counter("p2p_connect_ok_total", "Successful CONNECT coordinations", connect_ok_.load());
    counter("p2p_connect_fail_total", "Failed CONNECT coordinations", connect_fail_.load());
    counter("p2p_login_ok_total", "Successful AUTH_LOGIN", login_ok_.load());
    counter("p2p_login_fail_total", "Failed AUTH_LOGIN", login_fail_.load());
    counter("p2p_flood_drop_total", "Packets dropped by flood jail",
            abuse_.dropped_flood_pkts());
    counter("p2p_relay_reject_total", "MSG_PROXY_RELAY_DATA dropped by signaling core",
            relay_reject_.load());
    os << "# HELP p2p_connect_punch_total CONNECT punch admission (EasyTier policy)\n"
       << "# TYPE p2p_connect_punch_total counter\n"
       << "p2p_connect_punch_total{strategy=\"ice\"} " << punch_ice_.load() << "\n"
       << "p2p_connect_punch_total{strategy=\"relay\"} " << punch_relay_.load() << "\n"
       << "p2p_connect_punch_total{strategy=\"unknown\"} " << punch_unknown_.load()
       << "\n";
    os << "# HELP p2p_stun_reply_total STUN Binding replies (EasyTier responder)\n"
       << "# TYPE p2p_stun_reply_total counter\n"
       << "p2p_stun_reply_total{src=\"same\"} " << natcheck_.stun_same() << "\n"
       << "p2p_stun_reply_total{src=\"other\"} " << natcheck_.stun_other()
       << "\n";
    const std::string inst = json_safe_name(cfg ? cfg->instance_name : "");
    os << "# HELP p2p_node_region Node REGION label (1=set)\n"
       << "# TYPE p2p_node_region gauge\n"
       << "p2p_node_region{region=\"" << (region ? std::string(1, region) : "") << "\"} 1\n";
    os << "# HELP p2p_node_instance Node instance name (1=set)\n"
       << "# TYPE p2p_node_instance gauge\n"
       << "p2p_node_instance{instance=\"" << inst << "\"} 1\n";
    os << "# HELP p2p_proxy_used Current relay sessions on a proxy\n"
       << "# TYPE p2p_proxy_used gauge\n";
    relays_.visit([&](const ProxyHealth& p) {
        os << "p2p_proxy_used{ip=\"" << p.ip << "\"} " << p.used << "\n";
    });
    os << "# HELP p2p_proxy_up Proxy liveness (1=fresh and available)\n"
       << "# TYPE p2p_proxy_up gauge\n";
    {
        time_t now = time(nullptr);
        relays_.visit([&](const ProxyHealth& p) {
            const int up = (now - p.last_seen <= 30 && p.available && p.down < 2) ? 1 : 0;
            os << "p2p_proxy_up{ip=\"" << p.ip << "\"} " << up << "\n";
        });
    }
    return os.str();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s <NatServerPort> <ProxyServerPort> <WanIP> [P2pServers.cfg]\n"
               "  覆盖顺序：文件 < P2P_* 环境变量 < 本命令行端口\n"
               "  env P2P_STATUS_PORT=NNN     启用 HTTP 状态服务（GET / JSON，GET /metrics）\n"
               "  env P2P_STATUS_ALLOW=cidr   状态口来源白名单（空=不限制）\n"
               "  env P2P_INSTANCE_NAME=name  实例名（状态 JSON instance 字段）\n",
               argv[0]);
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
