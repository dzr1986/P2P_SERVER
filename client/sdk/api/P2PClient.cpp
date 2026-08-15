#include "client/sdk/api/P2PClient.h"
#include "common/IceSdp.h"
#include "common/ConnectToken.h"
#include "common/NatMatrix.h"
#include "common/Packet.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace p2p {

namespace {

inline uint64_t addr_key(const sockaddr_in& a) {
    return ((uint64_t)(uint32_t)ntohl(a.sin_addr.s_addr) << 16) |
           (uint64_t)ntohs(a.sin_port);
}

inline bool is_private_ip(const char* ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 127) return true;
    return false;
}

inline bool is_loopback_host(const std::string& ip) {
    return ip == "127.0.0.1" || ip == "localhost" || ip == "::1";
}

} // namespace

P2PClient::Conn* P2PClient::conn_of(const std::string& uuid) {
    auto it = conns_.find(uuid);
    return it == conns_.end() ? nullptr : it->second.get();
}

P2PClient::Conn& P2PClient::ensure_conn(const std::string& uuid) {
    auto it = conns_.find(uuid);
    if (it == conns_.end()) {
        auto c = std::make_unique<Conn>();
        c->peer_uuid = uuid;
        c->self = this;
        it = conns_.emplace(uuid, std::move(c)).first;
    }
    return *it->second;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool P2PClient::start(const Config& cfg) {
    if (running_.load()) return false;
    if (cfg.uuid.empty() || cfg.uuid.size() > MAX_UUID_LEN || cfg.nat_servers.empty())
        return false;

    cfg_ = cfg;
    secret_ = cfg.secret;
    nat_server_ = cfg.nat_servers[0];
    if (!sockaddr_from(nat_server_.ip, nat_server_.port, nat_sock_)) return false;

    if (!sock_.open(0, "0.0.0.0")) return false;
    portmap_pending_.clear();
    portmap_ok_.clear();
    next_portmap_ms_ = 0;
    if (cfg_.port_map) {
        const char* dis = getenv("P2P_DISABLE_PORTMAP");
        if (dis && dis[0] == '1') cfg_.port_map = false;
    }
    if (cfg_.port_map) {
        const uint16_t lp = sock_.local_port();
        if (lp) portmap_pending_.push_back(lp);
    }

    // P1：鉴权凭据 = 每 UID AuthKey（优先取配置的 hex，否则从主密钥派生）
    memset(auth_key_, 0, sizeof(auth_key_));
    has_cred_ = false;
    if (!cfg.auth_key_hex.empty()) {
        if (!auth_key_from_hex(cfg.auth_key_hex, auth_key_)) return false;
        has_cred_ = true;
    } else if (!secret_.empty()) {
        uid_derive_auth_key((const uint8_t*)secret_.data(), secret_.size(),
                            cfg_.uuid.c_str(), auth_key_);
        has_cred_ = true;
    }

    nat_type_ = NAT_UNKNOWN;
    nat_mapping_ = NAT_MAP_UNKNOWN;
    nat_filter_ = NAT_FLT_UNKNOWN;
    nat_port_step_ = 0;
    authed_ = !has_cred_;            // 无凭据视为免鉴权
    heartbeat_enc_ = false;
    auth_denied_ = false;
    auth_inflight_ = false;
    tunnel_enc_ = !secret_.empty();  // 共享密钥即开启对端间隧道负载加密
    tunnel_keys_.clear();
    hs_.clear();
    fs_keys_.clear();
    tunnel_fs_ok_.store(0);
    relay_registered_ = false;
    heartbeat_fail_ = 0;
    alt_port_ = 0;
    memset(pub_ip_, 0, sizeof(pub_ip_));
    pub_port_ = 0;

    proxies_ = cfg.proxy_servers;        // 启动即可用命令行/配置中的中继
    next_relay_reg_ = proxies_.empty() ? 0 : plat_now_ms();
    derp_registered_.store(false);
    derp_rbuf_.clear();
    derp_addr_ = {};
    next_derp_try_ms_ = 0;
    derp_backoff_ms_ = 400;
    derp_use_tls_ = cfg.proxy_tcp_tls;
    if (cfg.proxy_tcp_port != 0) {
        std::string ip = !cfg.proxy_servers.empty() ? cfg.proxy_servers[0].ip
                         : (!cfg.nat_servers.empty() ? cfg.nat_servers[0].ip : "");
        printf("[P2PClient] derp connect %s:%u tls=%d\n",
               ip.c_str(), (unsigned)cfg.proxy_tcp_port, cfg.proxy_tcp_tls ? 1 : 0);
        fflush(stdout);
        if (!ip.empty()) note_proxy_tcp(ip, cfg.proxy_tcp_port);
    }
    running_.store(true);
    pending_remote_sdp_.clear();
    lan_cache_.clear();
    lan_enabled_ = false;
    if (cfg_.lan_discover) {
        if (lan_sock_.open(LAN_MCAST_PORT, "0.0.0.0", true)) {
            lan_sock_.enable_broadcast();
            lan_sock_.join_multicast(LAN_MCAST_IP);
            lan_enabled_ = true;
            next_lan_announce_ms_ = plat_now_ms() + 150;
        }
    }
    thread_ = std::thread([this] { worker_loop(); });
    return true;
}

void P2PClient::stop() {
    if (!running_.exchange(false)) return;
    // 尽力注销中继
    if (relay_registered_ && !proxies_.empty()) {
        ProxyRegReq req;
        memset(&req, 0, sizeof(req));
        strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
        sockaddr_in to;
        if (sockaddr_from(proxies_[0].ip, proxies_[0].port, to))
            send_proto(MSG_PROXY_UNREGISTER_REQ, &req, sizeof(req), to);
    }
    if (derp_registered_.load() || derp_fd_.valid()) {
        ProxyRegReq req;
        memset(&req, 0, sizeof(req));
        strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
        derp_send(MSG_PROXY_UNREGISTER_REQ, &req, sizeof(req));
    }
    if (thread_.joinable()) thread_.join();
    derp_close();
    sock_.close();
    lan_sock_.close();
    lan_enabled_ = false;
    lan_cache_.clear();
    std::vector<juice_agent_t*> leftover;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (auto& kv : conns_) {
            Conn& c = *kv.second;
            if (c.punch.juice) {
                leftover.push_back(c.punch.juice);
                c.punch.juice = nullptr;
            }
            c.self = nullptr;
        }
        leftover.insert(leftover.end(), juice_reap_.begin(), juice_reap_.end());
        juice_reap_.clear();
        conns_.clear();
        sessions_.clear();
        addr_to_peer_.clear();
    }
    for (auto* j : leftover) juice_destroy(j);
}

// ---------------------------------------------------------------------------
// 外部接口
// ---------------------------------------------------------------------------
void P2PClient::connect(const std::string& peer_uuid, const std::string& token_hex) {
    if (!running_.load() || peer_uuid.empty() || peer_uuid == cfg_.uuid) return;
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto& c = ensure_conn(peer_uuid);
    if (c.state != ConnState::Idle) return;
    c.state = ConnState::Connecting;
    c.punch.have_direct = false;
    c.punch.direct_ok = false;
    c.punch.ice_gathered = false;
    c.punch.ice_host_sdp_sent = false;
    c.punch.ice_remote_gather_done = false;
    c.punch.ice_remote_applied_ms = 0;
    c.punch.ice_sdp_rtx_ms = 0;
    c.punch.ice_sdp_rtx_n = 0;
    c.punch.local_sdp.clear();
    c.punch.remote_sdp.clear();
    c.relay.relay_ok = false;
    c.punch.punch_deadline = plat_now_ms() + cfg_.connect_timeout_ms;
    c.punch.next_punch = 0;
    c.relay.next_relay_ping = 0;
    c.self = this; // #19 reverse ptr for ICE callback
    c.connect_token_hex = !token_hex.empty() ? token_hex : cfg_.connect_token_hex;

    // 只建 agent，等 CONNECT_OK（对端已收到 INVITE）再 gather：
    // 1) 鉴权/Token 被拒时不会先把过期 SDP 发给对端
    // 2) 发起方 gather = controlling；被邀方先 set_remote 再 gather = controlled
    ensure_ice_agent(c, false);
    send_connect_req(c);
    if (lan_enabled_) send_lan_beacon(MSG_LAN_QUERY, peer_uuid, nullptr);
}

void P2PClient::restart_ice(const std::string& peer_uuid) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Conn* c = conn_of(peer_uuid);
    if (!c || cfg_.force_relay) return;
    begin_ice_restart(*c, true);
}

void P2PClient::reap_juice(juice_agent_t*& agent) {
    if (!agent) return;
    juice_reap_.push_back(agent);
    agent = nullptr;
}

void P2PClient::reset_ice_flags(Conn& c) {
    c.punch.ice_gathered = false;
    c.punch.ice_host_sdp_sent = false;
    c.punch.ice_remote_gather_done = false;
    c.punch.ice_remote_applied_ms = 0;
    c.punch.ice_sdp_rtx_ms = 0;
    c.punch.ice_sdp_rtx_n = 0;
    c.punch.ice_nominated = false;
    c.punch.local_sdp.clear();
    c.punch.remote_sdp.clear();
    c.punch.direct_ok = false;
}

void P2PClient::begin_ice_restart(Conn& c, bool as_offerer) {
    if (cfg_.force_relay) return;
    if (c.punch.juice) {
        if (c.punch.juice_prev) reap_juice(c.punch.juice_prev);
        c.punch.juice_prev = c.punch.juice;
        c.punch.juice = nullptr;
    }
    reset_ice_flags(c);
    c.punch.ice_restarting = true;
    c.punch.ice_gen++;
    ensure_ice_agent(c, as_offerer);
    fprintf(stderr, "[P2PClient] ICE restart %s %s gen=%u\n",
            as_offerer ? "offer" : "answer", c.peer_uuid.c_str(),
            (unsigned)c.punch.ice_gen);
}

void P2PClient::disconnect(const std::string& peer_uuid) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Conn* c = conn_of(peer_uuid);
    if (!c) return;
    if (auto* s = session_for(peer_uuid)) {
        std::vector<uint8_t> frame(14);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_CLOSE, s->id(),
                           0, 0, 0, 0, nullptr, 0);
        send_tunnel_via(*c, frame.data(), frame.size());
    }
    close_conn(*c);
}

int P2PClient::send(const std::string& peer_uuid, uint8_t channel,
                    const void* data, size_t len, bool reliable) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Conn* c = conn_of(peer_uuid);
    if (!c || !c->connected()) return -1;
    return session_for(peer_uuid)->send(channel, data, len, reliable);
}

bool P2PClient::link_stats(const std::string& peer_uuid, Session::LinkStats& out) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = sessions_.find(peer_uuid);
    if (it == sessions_.end()) return false;
    out = it->second->stats();
    return true;
}

std::vector<P2PClient::LanPeer> P2PClient::lan_peers() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<LanPeer> out;
    out.reserve(lan_cache_.size());
    for (const auto& kv : lan_cache_) {
        LanPeer p;
        p.uuid = kv.first;
        p.ip = sockaddr_ip(kv.second.addr);
        p.port = sockaddr_port(kv.second.addr);
        out.push_back(std::move(p));
    }
    return out;
}

bool P2PClient::lan_lookup(const std::string& uuid, LanPeer& out) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = lan_cache_.find(uuid);
    if (it == lan_cache_.end()) return false;
    out.uuid = it->first;
    out.ip = sockaddr_ip(it->second.addr);
    out.port = sockaddr_port(it->second.addr);
    return true;
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------
void P2PClient::worker_loop() {
    uint64_t now = plat_now_ms();
    next_heartbeat_ms_ = now + 100;
    if (has_cred_) {
        do_auth_challenge();          // 优先完成鉴权再注册
        next_heartbeat_ms_ = now + 500;
    }

    while (running_.load()) {
        // #17 聚合全局信令 socket 与所有 Conn 的打洞 socket 收包
        fd_set rfds;
        FD_ZERO(&rfds);
        // select() 的 nfds 必须是最大 fd + 1，否则主信令 socket 永远进不了可读集合
        int maxfd = sock_.fd() + 1;
        FD_SET(sock_.fd(), &rfds);
        if (lan_enabled_ && lan_sock_.fd() >= 0) {
            FD_SET(lan_sock_.fd(), &rfds);
            if (lan_sock_.fd() + 1 > maxfd) maxfd = lan_sock_.fd() + 1;
        }
        std::vector<std::pair<std::string, int>> punch_fds; // peer_uuid -> socket fd
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            for (auto& kv : conns_) {
                for (auto& ps : kv.second->punch.punch_socks) {
                    int fd = ps.fd();
                    if (fd < 0) continue;
                    FD_SET(fd, &rfds);
                    if (fd + 1 > maxfd) maxfd = fd + 1;
                    punch_fds.emplace_back(kv.first, fd);
                }
            }
        }
        if (derp_fd_.valid()) {
            FD_SET(derp_fd_.fd(), &rfds);
            if (derp_fd_.fd() + 1 > maxfd) maxfd = derp_fd_.fd() + 1;
        }
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
        int r = select(maxfd, &rfds, nullptr, nullptr, &tv);
        now = plat_now_ms();
        if (r > 0) {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (derp_fd_.valid() && FD_ISSET(derp_fd_.fd(), &rfds))
                derp_on_readable();
            if (FD_ISSET(sock_.fd(), &rfds)) {
                sockaddr_in from;
                int n = sock_.recv_from(recv_buf_, sizeof(recv_buf_), from);
                if (n > 0) handle_packet(recv_buf_, (size_t)n, from);
            }
            if (lan_enabled_ && lan_sock_.fd() >= 0 && FD_ISSET(lan_sock_.fd(), &rfds)) {
                sockaddr_in from;
                int n = lan_sock_.recv_from(recv_buf_, sizeof(recv_buf_), from);
                if (n > 0) handle_packet(recv_buf_, (size_t)n, from);
            }
            for (auto& pf : punch_fds) {
                if (FD_ISSET(pf.second, &rfds)) {
                    auto it = conns_.find(pf.first);
                    if (it == conns_.end()) continue;
                    Conn& c = *it->second;
                    for (size_t si = 0; si < c.punch.punch_socks.size(); si++) {
                        auto& ps = c.punch.punch_socks[si];
                        if (ps.fd() != pf.second) continue;
                        sockaddr_in from;
                        int n = ps.recv_from(recv_buf_, sizeof(recv_buf_), from);
                        if (n > 0) {
                            // 记录直连确认所用的打洞 socket 索引
                            if (c.punch.direct_sock_idx < 0 &&
                                addr_to_peer_.count(addr_key(from)))
                                c.punch.direct_sock_idx = (int)si;
                            handle_packet(recv_buf_, (size_t)n, from);
                        }
                    }
                }
            }
        }
        std::vector<juice_agent_t*> reap;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            tick(now);
            reap.swap(juice_reap_);
        }
        for (auto* j : reap) juice_destroy(j);
    }
}

void P2PClient::tick(uint64_t now) {
    // 按阶段驱动状态机（各处理器职责单一、行为与原实现等价）
    tick_heartbeat(now);
    tick_auth(now);
    tick_nat_detect(now);
    tick_relay(now);
    tick_portmap(now);
    tick_connections(now);
    tick_sessions(now);
    tick_handshake(now);
    tick_lan(now);
    tick_ice(now);
}

// 心跳发送与重试间隔
void P2PClient::tick_heartbeat(uint64_t now) {
    if (now >= next_heartbeat_ms_) {
        do_heartbeat();
        uint32_t gap;
        if (heartbeat_fail_ > 0) {
            gap = heartbeat_backoff_.next_delay();   // #18 指数退避
        } else {
            heartbeat_backoff_.reset();
            gap = cfg_.heartbeat_ms;
        }
        next_heartbeat_ms_ = now + gap;
    }
}

// 鉴权状态机（challenge 重试）
void P2PClient::tick_auth(uint64_t now) {
    if (!authed_ && has_cred_ && !auth_inflight_ && !auth_denied_ &&
        now >= next_auth_try_) {
        do_auth_challenge();
    }
}

// NAT 检测推进
void P2PClient::tick_nat_detect(uint64_t now) {
    if (nat_detect_.done() && nat_type_ == NAT_UNKNOWN) {
        nat_type_ = nat_detect_.nattype();
        nat_mapping_ = nat_detect_.mapping();
        nat_filter_ = nat_detect_.filter();
        nat_port_step_ = nat_detect_.port_step();
        printf("[P2PClient] NAT type=%u mapping=%s filter=%s nat4e_step=%d\n",
               (unsigned)nat_type_, nat_mapping_str(nat_mapping_),
               nat_filter_str(nat_filter_), (int)nat_port_step_);
        fflush(stdout);
    }
    if (!nat_detect_.done()) {
        auto sendfn = [this](const sockaddr_in& to, const uint8_t* b, size_t n) {
            return sock_.send_to(b, n, to);
        };
        nat_detect_.tick(sendfn, nat_server_.ip.c_str(), nat_server_.port,
                         alt_port_, now);
    }
}

// 中继注册重试
void P2PClient::tick_relay(uint64_t now) {
    if (cfg_.auto_relay && !relay_registered_ && !proxies_.empty() &&
        now >= next_relay_reg_) {
        relay_register();
        next_relay_reg_ = now + cfg_.relay_register_ms;
    }
    if (!derp_registered_.load() && derp_addr_.port != 0 && !derp_fd_.valid() &&
        now >= next_derp_try_ms_)
        derp_try_connect();
}

void P2PClient::tick_portmap(uint64_t now) {
    if (!cfg_.port_map || portmap_pending_.empty() || now < next_portmap_ms_) return;
    const uint16_t port = portmap_pending_.front();
    portmap_pending_.erase(portmap_pending_.begin());
    if (portmap_ok_.count(port)) return;
    PortMapResult r;
    if (portmap_any(port, 3600, r)) {
        portmap_ok_[port] = r;
        printf("[P2PClient] portmap %s %u->%u lifetime=%us\n",
               r.backend, (unsigned)r.internal_port, (unsigned)r.external_port,
               (unsigned)r.lifetime_sec);
        fflush(stdout);
        next_portmap_ms_ = now + 80;
    } else {
        next_portmap_ms_ = now + 400;
    }
}

// 连接状态机（打洞/超时/降级中继；Connected+中继时后台继续打洞以便回切 P2P）
void P2PClient::tick_connections(uint64_t now) {
    // 先拷贝 key：tick_* 可能 close_conn 擦掉 unique_ptr，不能边遍历边删
    std::vector<std::string> peers;
    peers.reserve(conns_.size());
    for (const auto& kv : conns_) peers.push_back(kv.first);
    for (const auto& peer : peers) {
        Conn* c = conn_of(peer);
        if (!c) continue;
        if (c->state == ConnState::Connecting) {
            tick_conn_punch(*c, now);
            if (!(c = conn_of(peer))) continue;
            tick_conn_relay(*c, now);
            if (!(c = conn_of(peer))) continue;
            tick_conn_fsm(*c, now);
        } else if (c->state == ConnState::Connected && c->via_relay && !cfg_.force_relay) {
            tick_conn_punch(*c, now);
        }
    }
}

// 打洞子状态机：周期发包与超时判定
void P2PClient::tick_conn_punch(Conn& c, uint64_t now) {
    const bool upgrade = (c.state == ConnState::Connected && c.via_relay && !cfg_.force_relay);
    if (c.punch.have_direct && !c.punch.direct_ok && now >= c.punch.next_punch &&
        (now < c.punch.punch_deadline || upgrade)) {
        do_punch(c);
        c.punch.next_punch = now + cfg_.punch_interval_ms;
    }
    if (upgrade) return;  // 中继已通，后台打洞失败不关连接
    // force_relay 时 have_direct=false 是预期行为，不视为“服务器无响应”
    if (!cfg_.force_relay && !c.punch.have_direct &&
        now >= c.punch.punch_deadline + cfg_.connect_timeout_ms) {
        // 始终未拿到对端地址（服务器无响应/离线）
        if (on_error) on_error("connect " + c.peer_uuid + ": server no response");
        close_conn(c);
    }
}

// 中继子状态机：超时降级与保活
void P2PClient::tick_conn_relay(Conn& c, uint64_t now) {
    // ICE 仍在检查时不要按 6s 拆 agent：juice_destroy 曾与回调抢 mu_ 死锁，
    // 且 IOTC_Connect 等待 15s，过早 close 会让直连永远完不成。
    const bool derp_up = derp_registered_.load();
    // TCP/TLS 已通：立刻走中继出图，ICE 继续在后台（DERP 先通再切）
    if (c.punch.juice && !c.punch.direct_ok && !cfg_.force_relay && !derp_up &&
        now < c.punch.punch_deadline + cfg_.connect_timeout_ms) {
        return;
    }
    const bool need_relay = cfg_.force_relay || derp_up ||
        (c.punch.have_direct && now >= c.punch.punch_deadline && !c.punch.direct_ok);
    if (need_relay) {
        // 打洞超时或强制中继：走中继
        if (cfg_.auto_relay && !relay_registered_ && !proxies_.empty()) {
            relay_register();
            // #18 中继注册重试指数退避（base=relay_register_ms, cap=30s）
            uint32_t d = cfg_.relay_register_ms;
            for (uint32_t i = 0; i + 1 < c.backoff_attempt && d < 30000; i++) d *= 2;
            if (d > 30000) d = 30000;
            c.backoff_attempt++;
            next_relay_reg_ = now + d;
        }
        if (cfg_.auto_relay && !proxies_.empty() && now >= c.relay.next_relay_ping) {
            if (auto* s = session_for(c.peer_uuid)) {
                std::vector<uint8_t> frame(14);
                codec_write_tunnel(frame.data(), (int)frame.size(), TT_PING,
                                   s->id(), 0, 0, 0, 0, nullptr, 0);
                send_tunnel_via(c, frame.data(), frame.size());
            }
            c.relay.next_relay_ping = now + cfg_.punch_interval_ms;
        } else if (!cfg_.force_relay && (!cfg_.auto_relay || proxies_.empty()) &&
                   c.punch.have_direct && now >= c.punch.punch_deadline && !c.punch.direct_ok) {
            // 无中继可用：判失败（force_relay 时继续等代理列表）
            if (on_error) on_error("connect " + c.peer_uuid + ": no relay, direct failed");
            close_conn(c);
        }
    }
}

// 连接主状态机：中继路径建立判定
void P2PClient::tick_conn_fsm(Conn& c, uint64_t /*now*/) {
    if (c.relay.relay_ok) set_connected(c, true);
}

// 连接建立唯一转移点（幂等）：Connecting -> Connected；
// 已 Connected 且中继路径上直连打通时无缝切回 P2P（再触发一次 on_connected(relay=false)）
void P2PClient::set_connected(Conn& c, bool relay) {
    if (c.state == ConnState::Connected) {
        if (c.via_relay && !relay) {
            c.via_relay = false;
            if (on_connected) on_connected(c.peer_uuid, false);
        } else if (!relay && c.punch.ice_restarting) {
            c.punch.ice_restarting = false;
            ice_restarts_.fetch_add(1);
            fprintf(stderr, "[P2PClient] ICE restarted with %s\n", c.peer_uuid.c_str());
            if (on_connected) on_connected(c.peer_uuid, false);
        }
        return;
    }
    c.state = ConnState::Connected;
    c.via_relay = relay;
    // 握手放到 tick_handshake，避免在 libjuice 状态回调里 juice_send
    if (on_connected) on_connected(c.peer_uuid, relay);
}

void P2PClient::start_handshake(const std::string& peer) {
    auto& h = hs_[peer];
    if (!h.local_ready) {
        if (x25519_keypair(h.pub, h.priv) != 0) return;
        if (p2p_random_bytes(h.nonce, sizeof(h.nonce)) != 0) return;
        h.local_ready = true;
        h.last_rekey_ms = plat_now_ms();
    }
    uint8_t msg[HS_LEN];
    const uint8_t* psk = secret_.empty() ? nullptr : (const uint8_t*)secret_.data();
    if (hs_write(msg, sizeof(msg), h.pub, h.nonce, psk, secret_.size()) == 0) return;
    // 不可靠发送，避免占用可靠序号把头阻塞 IOCtrl/RDT
    if (auto* s = session_for(peer)) s->send(HS_CHANNEL, msg, HS_LEN, false);
    if (h.remote_ready) finish_handshake(peer);
}

void P2PClient::on_hs_msg(const std::string& peer, const uint8_t* data, size_t len) {
    uint8_t pub[32], nonce[16];
    const uint8_t* psk = secret_.empty() ? nullptr : (const uint8_t*)secret_.data();
    if (!hs_read(data, len, pub, nonce, psk, secret_.size()) &&
        !hs_read(data, len, pub, nonce, nullptr, 0)) {
        return;
    }
    auto& h = hs_[peer];
    if (data[2] == HS_ACK) {
        h.peer_acked = true;
        return;
    }
    if (h.remote_ready && memcmp(h.peer_pub, pub, 32) == 0 &&
        memcmp(h.peer_nonce, nonce, 16) == 0) {
        return;   // 重复 HELLO
    }
    if (h.done && memcmp(h.peer_pub, pub, 32) != 0) {
        h.done = false;
        h.peer_acked = false;
        h.local_ready = false;   // 对端轮换：本端同步换新临时密钥
    }
    memcpy(h.peer_pub, pub, 32);
    memcpy(h.peer_nonce, nonce, 16);
    h.remote_ready = true;
    if (!h.local_ready) start_handshake(peer);
    else finish_handshake(peer);
}

void P2PClient::finish_handshake(const std::string& peer) {
    auto& h = hs_[peer];
    if (!h.local_ready || !h.remote_ready) return;
    uint8_t shared[32];
    if (x25519(shared, h.priv, h.peer_pub) != 0) return;
    uint8_t key[32];
    hs_derive_session_key(shared, h.pub, h.nonce, h.peer_pub, h.peer_nonce,
                          cfg_.uuid.c_str(), peer.c_str(), key);
    memset(h.priv, 0, sizeof(h.priv));
    memset(shared, 0, sizeof(shared));
    auto& fs = fs_keys_[peer];
    if (fs.has_cur) {
        fs.prev = fs.cur;
        fs.has_prev = true;
    }
    memcpy(fs.cur.data(), key, 32);
    fs.has_cur = true;
    tunnel_enc_ = true;
    if (!h.done) tunnel_fs_ok_.fetch_add(1);
    h.done = true;
    h.last_rekey_ms = plat_now_ms();
    // 通知对端：本端已派生，对端可以切换 FS 加密
    uint8_t ack[HS_LEN];
    const uint8_t* psk = secret_.empty() ? nullptr : (const uint8_t*)secret_.data();
    if (hs_write(ack, sizeof(ack), h.pub, h.nonce, psk, secret_.size(), HS_ACK) != 0) {
        if (auto* s = session_for(peer)) s->send(HS_CHANNEL, ack, HS_LEN, false);
    }
}

void P2PClient::tick_handshake(uint64_t now) {
    for (auto& kv : conns_) {
        if (!kv.second->connected()) continue;
        auto it = hs_.find(kv.first);
        if (it == hs_.end() || !it->second.local_ready) {
            start_handshake(kv.first);
            continue;
        }
        if (!it->second.done || !it->second.peer_acked) {
            if (now - it->second.last_rekey_ms >= 200) {
                it->second.last_rekey_ms = now;
                uint8_t msg[HS_LEN];
                const uint8_t* psk = secret_.empty() ? nullptr : (const uint8_t*)secret_.data();
                if (hs_write(msg, sizeof(msg), it->second.pub, it->second.nonce,
                             psk, secret_.size()) != 0) {
                    if (auto* s = session_for(kv.first))
                        s->send(HS_CHANNEL, msg, HS_LEN, false);
                }
            }
            continue;
        }
        if (it->second.done && now - it->second.last_rekey_ms >= 180000) {
            it->second.local_ready = false;
            it->second.done = false;
            it->second.peer_acked = false;
            start_handshake(kv.first);
        }
    }
}

// 会话周期驱动
void P2PClient::tick_sessions(uint64_t now) {
    for (auto& kv : sessions_) kv.second->tick(now);
}

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------
void P2PClient::send_proto(uint8_t msg_id, const void* payload, size_t plen,
                           const sockaddr_in& to) {
    uint8_t buf[MAX_PKT];
    if (plen + 8 > sizeof(buf)) return;
    codec_write_head(buf, msg_id, (uint32_t)plen);
    if (plen > 0 && payload) memcpy(buf + 8, payload, plen);
    sock_.send_to(buf, 8 + plen, to);
}

void P2PClient::send_proto(uint8_t msg_id, const void* payload, size_t plen) {
    send_proto(msg_id, payload, plen, nat_sock_);
}

void P2PClient::send_connect_req(const Conn& c) {
    ConnectReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.src_uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    strncpy(req.dst_uuid, c.peer_uuid.c_str(), MAX_UUID_LEN);
    if (c.connect_token_hex.empty()) {
        send_proto(MSG_CONNECT_REQ, &req, sizeof(req));
        return;
    }
    ConnectToken tok{};
    if (!connect_token_from_hex(c.connect_token_hex, tok)) {
        if (on_error) on_error("invalid connect token hex");
        return;
    }
    uint8_t buf[sizeof(ConnectReq) + sizeof(ConnectToken)];
    memcpy(buf, &req, sizeof(req));
    memcpy(buf + sizeof(req), &tok, sizeof(tok));
    send_proto(MSG_CONNECT_REQ, buf, sizeof(buf));
}

void P2PClient::handle_packet(const uint8_t* buf, size_t len,
                              const sockaddr_in& from) {
    WireHead h;
    if (codec_read_head(buf, len, h)) {
        handle_proto(h.msg_id, buf + 8, h.length <= len - 8 ? h.length : 0, from);
        return;
    }
    if (len >= 2 && buf[0] == (uint8_t)(TUNNEL_MAGIC >> 8) &&
        buf[1] == (uint8_t)(TUNNEL_MAGIC & 0xFF)) {
        // 直连隧道帧
        auto it = addr_to_peer_.find(addr_key(from));
        if (it != addr_to_peer_.end()) {
            Conn* c = conn_of(it->second);
            if (!c) return;
            on_tunnel_frame(buf, len, it->second);
            if (!c->punch.direct_ok) {
                c->punch.direct_ok = true;
                set_connected(*c, false);
            }
        }
        return;
    }
    // 未知报文，忽略
}

void P2PClient::handle_proto(uint8_t msg_id, const uint8_t* p, size_t plen,
                             const sockaddr_in& from) {
    switch (msg_id) {
    case MSG_HEARTBEAT_RSP:          on_heartbeat_rsp(p, plen); break;
    case MSG_HEARTBEAT_RSP_ENC:      on_heartbeat_rsp_enc(p, plen); break;
    case MSG_AUTH_CHALLENGE_RSP:     on_auth_challenge_rsp(p, plen); break;
    case MSG_AUTH_LOGIN_RSP:         on_auth_login_rsp(p, plen); break;
    case MSG_NAT_DETECT_RSP: {
        nat_detect_.feed(p, plen);
        break;
    }
    case MSG_CONNECT_ACK:            on_connect_ack(p, plen); break;
    case MSG_CONNECT_INVITE:         on_connect_invite(p, plen); break;
    case MSG_ICE_SDP:                on_ice_sdp(p, plen); break;
    case MSG_LAN_QUERY:
    case MSG_LAN_ANNOUNCE:           on_lan_beacon(msg_id, p, plen, from); break;
    case MSG_PROXY_REGISTER_RSP:     on_proxy_register_rsp(p, plen); break;
    case MSG_PROXY_RELAY_DATA:       on_proxy_relay_data(p, plen); break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 心跳 / 注册
// ---------------------------------------------------------------------------
void P2PClient::do_heartbeat() {
    UuidReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    req.dev_type = 2;
    req.nattype = nat_type_;
    req.lan_port = htons(sock_.local_port());
    req.session_pts = htonl(1);
    req.extlen = 0;

    if (heartbeat_enc_) {
        // 加密心跳：uuid(33B) || iv(8) || cipher(UuidReq)
        uint8_t payload[33 + 8 + sizeof(UuidReq)];
        memset(payload, 0, sizeof(payload));
        memcpy(payload, cfg_.uuid.c_str(), cfg_.uuid.size());
        uint8_t iv[8];
        if (!plat_rand_bytes(iv, sizeof(iv))) {
            if (on_error) on_error("secure random unavailable for heartbeat iv");
            return;
        }
        memcpy(payload + 33, iv, sizeof(iv));
        memcpy(payload + 41, &req, sizeof(UuidReq));
        // P1：流密钥用每 UID AuthKey（与服务端派生对称）
        p2p_stream_xor(auth_key_, sizeof(auth_key_),
                       (const char*)payload, iv, payload + 41, sizeof(UuidReq));
        send_proto(MSG_HEARTBEAT_REQ_ENC, payload, sizeof(payload));
        return;
    }
    send_proto(MSG_HEARTBEAT_REQ, &req, sizeof(req));
}

void P2PClient::on_heartbeat_rsp_enc(const uint8_t* p, size_t plen) {
    // 负载 = uuid(33B) || iv(8) || cipher(ExtInfoRsp)
    if (plen < 33 + 8 + 2 || plen - 41 > sizeof(ExtInfoRsp)) return;
    const uint8_t* iv = p + 33;
    size_t clen = plen - 41;
    uint8_t body[sizeof(ExtInfoRsp)];
    memcpy(body, p + 41, clen);
    p2p_stream_xor(auth_key_, sizeof(auth_key_), (const char*)p, iv, body, clen);
    on_heartbeat_rsp(body, clen);
}

void P2PClient::on_heartbeat_rsp(const uint8_t* p, size_t plen) {
    if (plen < 2) return;
    uint8_t result = p[0];
    if (result == 1) {                    // 需鉴权
        if (!has_cred_) {
            if (on_error) on_error("server requires auth but no secret configured");
        } else if (!auth_inflight_ && !auth_denied_) {
            do_auth_challenge();
        }
        return;
    }
    if (result == 2) {
        if (on_error) on_error("server rejected this uuid");
        return;
    }
    if (plen < sizeof(ExtInfoRsp)) return;
    heartbeat_fail_ = 0;
    heartbeat_backoff_.reset();   // #18 心跳成功，重置退避
    memcpy(pub_ip_, p + 1, MAX_IP_LEN);
    pub_port_ = (uint16_t)((p[17] << 8) | p[18]);
    alt_port_ = (uint16_t)((p[37] << 8) | p[38]);   // nat_sock2_port @37

    if (!authed_ && has_cred_ && !auth_inflight_ && !auth_denied_) {
        do_auth_challenge();               // 服务器未强制鉴权，但仍补做
        return;
    }
    if (!nat_detect_.done()) {
        auto sendfn = [this](const sockaddr_in& to, const uint8_t* b, size_t n) {
            return sock_.send_to(b, n, to);
        };
        nat_detect_.start(nat_server_.ip.c_str(), nat_server_.port, sendfn,
                          plat_now_ms());
    }
    if (on_ready && !ready_fired_) {
        ready_fired_ = true;
        on_ready(pub_ip_, pub_port_, nat_type_);
    }
}

// ---------------------------------------------------------------------------
// 鉴权
// ---------------------------------------------------------------------------
void P2PClient::do_auth_challenge() {
    AuthChallengeReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    send_proto(MSG_AUTH_CHALLENGE_REQ, &req, sizeof(req));
    auth_inflight_ = true;
}

void P2PClient::on_auth_challenge_rsp(const uint8_t* p, size_t plen) {
    if (plen < 19) { auth_inflight_ = false; next_auth_try_ = plat_now_ms() + auth_backoff_.next_delay(); return; }
    uint8_t result = p[0];
    if (result != 0) {
        auth_inflight_ = false;
        if (result == 1 && on_error) on_error("uuid blacklisted");
        else if (result == 2 && on_error) on_error("uuid not in whitelist");
        if (result == 1 || result == 2) {
            auth_denied_ = true;
            next_auth_try_ = plat_now_ms() + 3600000;   // 永久性拒绝：不再重试
        } else {
            next_auth_try_ = plat_now_ms() + auth_backoff_.next_delay();   // #18 指数退避
        }
        return;
    }
    memcpy(auth_nonce_, p + 1, 16);
    do_auth_login();
}

void P2PClient::do_auth_login() {
    // P1：mac = HMAC-SHA256(AuthKey, uuid(33B, 补零) || nonce(16B))
    //   AuthKey 为每 UID 独立密钥（uidgen 签发或由主密钥派生）
    uint8_t msg[16 + MAX_UUID_LEN + 1];
    memset(msg, 0, sizeof(msg));
    memcpy(msg, cfg_.uuid.c_str(), cfg_.uuid.size());
    memcpy(msg + MAX_UUID_LEN + 1, auth_nonce_, 16);

    AuthLoginReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    memcpy(req.nonce, auth_nonce_, 16);
    hmac_sha256(auth_key_, sizeof(auth_key_), msg, sizeof(msg), req.mac);
    send_proto(MSG_AUTH_LOGIN_REQ, &req, sizeof(req));
}

void P2PClient::on_auth_login_rsp(const uint8_t* p, size_t plen) {
    auth_inflight_ = false;
    if (plen < 1) return;
    uint8_t result = p[0];
    if (result == AUTH_OK) {
        authed_ = true;
        auth_backoff_.reset();               // #18 鉴权成功，重置退避
        heartbeat_enc_ = true;                // 后续心跳走加密通道
        next_heartbeat_ms_ = plat_now_ms() + 100;   // 鉴权成功立即心跳确认注册
        // 重发等待鉴权期间的连接请求
        for (auto& kv : conns_) {
            Conn& c = *kv.second;
            if (c.connecting() && !c.punch.have_direct && !c.relay.relay_ok) {
                send_connect_req(c);
                // #18 CONNECT 重发指数退避（下次若仍无响应由 tick 驱动）
                uint32_t d = cfg_.connect_timeout_ms;
                for (uint32_t i = 0; i + 1 < c.backoff_attempt && d < 30000; i++) d *= 2;
                if (d > 30000) d = 30000;
                c.backoff_attempt++;
            }
        }
    } else {
        if (result == AUTH_WHITELIST_REJ && on_error) on_error("uuid rejected by whitelist");
        else if (result == AUTH_BLACKLIST && on_error) on_error("uuid blacklisted");
        else if (result == AUTH_BAD_MAC && on_error) on_error("auth login failed (bad secret?)");
        else if (on_error) on_error("auth login failed");
        if (result != AUTH_BAD_NONCE) {
            auth_denied_ = true;                  // 永久性拒绝：不再重试
            next_auth_try_ = plat_now_ms() + 3600000;
        } else {
            next_auth_try_ = plat_now_ms() + auth_backoff_.next_delay();  // #18 nonce 失效指数退避
        }
    }
}

// ---------------------------------------------------------------------------
// CONNECT 协调
// ---------------------------------------------------------------------------
bool P2PClient::parse_proxies(uint8_t count, const ProxyCandidate* cands,
                              std::vector<ServerAddr>& out) {
    if (count == 0 || !cands) return false;
    for (int i = 0; i < count && i < 3; i++) {
        ServerAddr a;
        a.ip = std::string(cands[i].ip, strnlen(cands[i].ip, MAX_IP_LEN));
        a.port = ntohs(cands[i].port);
        if (a.ip.empty() || a.port == 0) continue;
        bool dup = false;
        for (auto& x : out) if (x.ip == a.ip && x.port == a.port) { dup = true; break; }
        if (!dup) out.push_back(a);
    }
    return !out.empty();
}

void P2PClient::on_proxy_register_rsp(const uint8_t* p, size_t plen) {
    if (plen < 1) return;
    uint8_t result = p[0];
    if (result == 0) {
        relay_registered_ = true;
        for (auto& kv : conns_) kv.second->backoff_attempt = 0;
    } else if (on_error) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "relay register failed result=%d", result);
        on_error(tmp);
    }
}

void P2PClient::on_proxy_relay_data(const uint8_t* p, size_t plen) {
    if (plen < sizeof(RelayFrame) + 14) return;
    const char* src = (const char*)p;
    const char* dst = (const char*)p + MAX_UUID_LEN + 1;
    if (strncmp(dst, cfg_.uuid.c_str(), MAX_UUID_LEN) != 0) return;
    std::string peer_uuid(src, strnlen(src, MAX_UUID_LEN));
    if (peer_uuid.empty()) return;
    auto& c = ensure_conn(peer_uuid);
    on_tunnel_frame(p + sizeof(RelayFrame), plen - sizeof(RelayFrame), peer_uuid);
    if (!c.relay.relay_ok) {
        c.relay.relay_ok = true;
        set_connected(c, true);
    }
}

void P2PClient::ensure_punch_pool(Conn& c) {
    if (c.punch.juice || !c.punch.punch_socks.empty()) return;
    constexpr int kPunchSocks = 8;
    c.punch.punch_socks.reserve(kPunchSocks);
    for (int i = 0; i < kPunchSocks; i++) {
        UdpSocket s;
        if (!s.open(0, "0.0.0.0")) continue;
        c.punch.punch_socks.push_back(std::move(s));
    }
}

void P2PClient::on_connect_ack(const uint8_t* p, size_t plen) {
    if (plen < sizeof(ConnectAck)) return;
    ConnectAck ack;
    memcpy(&ack, p, sizeof(ConnectAck));
    std::string peer(ack.dst_uuid, strnlen(ack.dst_uuid, MAX_UUID_LEN));

    if (ack.result == CONNECT_NEED_AUTH) {
        if (has_cred_ && !auth_inflight_ && !auth_denied_) do_auth_challenge();
        return;
    }
    auto& c = ensure_conn(peer);
    if (c.state == ConnState::Idle) c.state = ConnState::Connecting;
    if (ack.result != CONNECT_OK) {
        if (ack.result == CONNECT_BAD_TOKEN) {
            if (on_error) on_error("connect " + peer + ": bad or expired connect token");
        } else if (on_error) {
            on_error("connect " + peer + ": peer offline/not found");
        }
        close_conn(c);
        return;
    }
    if (cfg_.force_relay) {
        c.punch.have_direct = false;   // 强制中继：不打洞
        c.punch.punch_deadline = plat_now_ms(); // 立即允许走中继发送
    } else {
        c.punch.have_direct = true;
        c.punch.next_punch = plat_now_ms() + 100;

        sockaddr_from(ack.dst_pub_ip, ntohs(ack.dst_pub_port), c.punch.direct);
        addr_to_peer_[addr_key(c.punch.direct)] = peer;
        // 私网目标同样尝试（同一局域网场景）
        c.punch.have_lan = false;
        if (is_private_ip(ack.dst_lan_ip) &&
            strncmp(ack.dst_lan_ip, ack.dst_pub_ip, MAX_IP_LEN) != 0) {
            if (sockaddr_from(ack.dst_lan_ip, ntohs(ack.dst_lan_port), c.punch.direct_lan)) {
                c.punch.have_lan = true;
                addr_to_peer_[addr_key(c.punch.direct_lan)] = peer;
            }
        }
        c.punch.punch_deadline = plat_now_ms() + cfg_.connect_timeout_ms;
        ensure_ice_agent(c, true);   // CONNECT 已成功：发起方 gather → controlling
        auto pit = pending_remote_sdp_.find(peer);
        if (pit != pending_remote_sdp_.end()) {
            apply_remote_ice_sdp(c, pit->second);
            pending_remote_sdp_.erase(pit);
        }
        auto lit = lan_cache_.find(peer);
        if (lit != lan_cache_.end()) {
            apply_lan_peer(peer, lit->second.addr, lit->second.addr.sin_port);
        }
        ensure_punch_pool(c);
    }

    std::vector<ServerAddr> cands;
    parse_proxies(ack.proxy_count, ack.proxies, cands);
    if (proxies_.empty() && !cands.empty()) {
        proxies_ = cands;
        next_relay_reg_ = plat_now_ms();
    } else if (proxies_.empty() && !cfg_.proxy_servers.empty()) {
        proxies_ = cfg_.proxy_servers;
        next_relay_reg_ = plat_now_ms();
    }
    const uint16_t tcp_port = ntohs(ack.proxy_tcp_port);
    if (tcp_port) {
        std::string ip = !cands.empty() ? cands[0].ip
                         : (!proxies_.empty() ? proxies_[0].ip : "");
        if (!ip.empty()) note_proxy_tcp(ip, tcp_port);
    }
}

void P2PClient::on_connect_invite(const uint8_t* p, size_t plen) {
    if (plen < sizeof(ConnectInvite)) return;
    ConnectInvite inv;
    memcpy(&inv, p, sizeof(ConnectInvite));
    std::string peer(inv.src_uuid, strnlen(inv.src_uuid, MAX_UUID_LEN));
    if (peer.empty()) return;

    Conn& c = ensure_conn(peer);
    if (c.state == ConnState::Idle) c.state = ConnState::Connecting;
    c.punch.direct_ok = false;
    c.self = this;
    if (cfg_.force_relay) {
        c.punch.have_direct = false;   // 强制中继：不打洞
        c.punch.punch_deadline = plat_now_ms();
    } else {
        c.punch.have_direct = true;
        c.punch.next_punch = plat_now_ms() + 100;
        c.punch.punch_deadline = plat_now_ms() + cfg_.connect_timeout_ms;

        sockaddr_from(inv.src_pub_ip, ntohs(inv.src_pub_port), c.punch.direct);
        addr_to_peer_[addr_key(c.punch.direct)] = peer;
        // 被邀方只建 agent、先等对端 SDP：set_remote 后再 gather → controlled
        ensure_ice_agent(c, false);
        auto pit = pending_remote_sdp_.find(peer);
        if (pit != pending_remote_sdp_.end()) {
            apply_remote_ice_sdp(c, pit->second);
            pending_remote_sdp_.erase(pit);
        }
        auto lit = lan_cache_.find(peer);
        if (lit != lan_cache_.end()) {
            apply_lan_peer(peer, lit->second.addr, lit->second.addr.sin_port);
        }
        ensure_punch_pool(c);
    }

    std::vector<ServerAddr> cands;
    parse_proxies(inv.proxy_count, inv.proxies, cands);
    if (proxies_.empty() && !cands.empty()) {
        proxies_ = cands;
        next_relay_reg_ = plat_now_ms();
    } else if (proxies_.empty() && !cfg_.proxy_servers.empty()) {
        proxies_ = cfg_.proxy_servers;
        next_relay_reg_ = plat_now_ms();
    }
    const uint16_t tcp_port = ntohs(inv.proxy_tcp_port);
    if (tcp_port) {
        std::string ip = !cands.empty() ? cands[0].ip
                         : (!proxies_.empty() ? proxies_[0].ip : "");
        if (!ip.empty()) note_proxy_tcp(ip, tcp_port);
    }
}

// ---------------------------------------------------------------------------
// #19 libjuice ICE 回调
// ---------------------------------------------------------------------------
void P2PClient::on_juice_state(juice_agent_t* agent, juice_state_t state, void* user_ptr) {
    auto* c = static_cast<Conn*>(user_ptr);
    if (!c) return;
    fprintf(stderr, "[P2PClient] ICE state=%s peer=%s\n",
            juice_state_to_string(state), c->peer_uuid.c_str());
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        P2PClient* self = c->self;
        if (!self) return;
        std::lock_guard<std::recursive_mutex> lk(self->mu_);
        if (c->self != self) return;  // 已被 close_conn 置空
        if (agent != c->punch.juice) return;  // 旧 agent 的状态忽略
        c->punch.direct_ok = true;
        c->punch.ice_nominated = true;
        if (c->punch.juice_prev) self->reap_juice(c->punch.juice_prev);
        self->set_connected(*c, false);
    }
}

void P2PClient::on_juice_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr) {
    auto* c = static_cast<Conn*>(user_ptr);
    if (!c || !sdp) return;
    P2PClient* self = c->self;
    if (!self) return;
    std::lock_guard<std::recursive_mutex> lk(self->mu_);
    if (c->self != self) return;
    if (agent != c->punch.juice) return;
    // 阶段 B：候选累积至 local_sdp（信令交换待扩展协议）
    if (!c->punch.local_sdp.empty()) c->punch.local_sdp += "\n";
    c->punch.local_sdp += sdp;
    // 公网：第一个 host 即可先发，不等 STUN。回环跳过——此时往往只有 eth0 等
    // 非 127.0.0.1 候选，对端 controlling 会打到打不通的地址；gather 无 STUN
    // 会立刻完成，由 on_juice_gathering_done 发齐全部 host。
    if (self->cfg_.port_map && sdp) {
        std::vector<uint16_t> ps;
        portmap_host_ports_from_sdp(sdp, ps);
        for (uint16_t p : ps) {
            if (!self->portmap_ok_.count(p)) self->portmap_pending_.push_back(p);
        }
    }
    if (sdp && strstr(sdp, "typ host") && !c->punch.ice_host_sdp_sent &&
        !is_loopback_host(self->nat_server_.ip)) {
        char full[JUICE_MAX_SDP_STRING_LEN] = {0};
        if (juice_get_local_description(agent, full, sizeof(full)) == JUICE_ERR_SUCCESS &&
            full[0]) {
            c->punch.ice_host_sdp_sent = true;
            c->punch.local_sdp = full;
            self->send_ice_sdp(c->peer_uuid, c->punch.local_sdp);
        }
    }
}

void P2PClient::on_juice_gathering_done(juice_agent_t* agent, void* user_ptr) {
    auto* c = static_cast<Conn*>(user_ptr);
    if (!c) return;
    P2PClient* self = c->self;
    if (!self) return;
    std::lock_guard<std::recursive_mutex> lk(self->mu_);
    if (c->self != self) return;
    if (agent != c->punch.juice) return;
    // gather 完成后取完整 local description（含全部候选），勿使用追加拼接的半成品
    char sdp[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(agent, sdp, sizeof(sdp)) == JUICE_ERR_SUCCESS)
        c->punch.local_sdp = sdp;
    self->send_ice_sdp(c->peer_uuid, c->punch.local_sdp);
}
void P2PClient::ensure_ice_agent(Conn& c, bool as_offerer) {
    if (cfg_.force_relay) return;
    if (!c.punch.juice) {
        juice_config_t jcfg {};
        jcfg.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
        jcfg.cb_state_changed = &P2PClient::on_juice_state;
        jcfg.cb_candidate = &P2PClient::on_juice_candidate;
        jcfg.cb_gathering_done = &P2PClient::on_juice_gathering_done;
        jcfg.cb_recv = &P2PClient::on_juice_recv;
        jcfg.user_ptr = &c;
        // 公网 NatServer 兼 STUN，收集 srflx。回环本机测试跳过：
        // juice STUN 重传合计约 23.5s，host 候选已足够同机直连。
        // 回环还把 juice socket 绑到 127.0.0.1，避免 host 候选落在 eth0/docker
        // 导致 controlling 单向打不通。
        if (!nat_server_.ip.empty() && !is_loopback_host(nat_server_.ip)) {
            jcfg.stun_server_host = nat_server_.ip.c_str();
            jcfg.stun_server_port = nat_server_.port;
        } else {
            static const char kLoopback[] = "127.0.0.1";
            jcfg.bind_address = kLoopback;
        }
        c.punch.juice = juice_create(&jcfg);
    }
    if (c.punch.juice && as_offerer) start_ice_gather(c);
}

void P2PClient::start_ice_gather(Conn& c) {
    if (!c.punch.juice || c.punch.ice_gathered || cfg_.force_relay) return;
    c.punch.ice_gathered = true;
    juice_gather_candidates(c.punch.juice);
}

void P2PClient::apply_remote_ice_sdp(Conn& c, const std::string& remote_sdp) {
    if (remote_sdp.empty()) return;
    if (!c.punch.remote_sdp.empty() && ice_sdp_is_restart(c.punch.remote_sdp, remote_sdp))
        begin_ice_restart(c, false);
    if (!c.punch.juice) ensure_ice_agent(c, false);
    if (!c.punch.juice) return;
    if (c.punch.remote_sdp.empty()) {
        c.punch.remote_sdp = remote_sdp;
        c.punch.ice_remote_applied_ms = plat_now_ms();
        // 被邀方：先 set_remote（mode 仍 UNKNOWN → controlled），再 gather
        // 首包多为 host-only，此时不可 mark gathering done，否则后续 srflx trickle 会被 juice 拒绝
        juice_set_remote_description(c.punch.juice, remote_sdp.c_str());
        if (!c.punch.ice_gathered) start_ice_gather(c);
        fprintf(stderr, "[P2PClient] ICE_SDP applied for %s (trickle open)\n",
                c.peer_uuid.c_str());
        return;
    }
    if (remote_sdp == c.punch.remote_sdp) {
        // gather 完成但没有新候选（回环/STUN 失败）：可以收口
        if (!c.punch.ice_remote_gather_done) {
            juice_set_remote_gathering_done(c.punch.juice);
            c.punch.ice_remote_gather_done = true;
            fprintf(stderr, "[P2PClient] ICE_SDP remote gathering done for %s (unchanged)\n",
                    c.peer_uuid.c_str());
        }
        return;
    }
    // 后续 SDP（STUN 完成后的 srflx）：按行 trickle 候选
    if (!c.punch.ice_remote_gather_done) {
        std::string line;
        for (size_t i = 0, n = remote_sdp.size(); i <= n; i++) {
            if (i < n && remote_sdp[i] != '\n') {
                line += remote_sdp[i];
                continue;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 11 && line.compare(0, 11, "a=candidate") == 0)
                juice_add_remote_candidate(c.punch.juice, line.c_str());
            line.clear();
        }
        juice_set_remote_gathering_done(c.punch.juice);
        c.punch.ice_remote_gather_done = true;
        fprintf(stderr, "[P2PClient] ICE_SDP trickle+done for %s\n", c.peer_uuid.c_str());
    }
    c.punch.remote_sdp = remote_sdp;
    c.punch.ice_remote_applied_ms = plat_now_ms();
}

void P2PClient::send_ice_sdp(const std::string& peer, const std::string& local_sdp) {
    if (peer.empty() || local_sdp.empty()) return;
    size_t plen = local_sdp.size();
    if (plen > 4096) plen = 4096;  // SDP 安全上限
    size_t msglen = sizeof(IceSdpMsg) - 1 + plen;
    std::vector<uint8_t> buf(msglen);
    IceSdpMsg* m = reinterpret_cast<IceSdpMsg*>(buf.data());
    strncpy(m->dst_uuid, peer.c_str(), MAX_UUID_LEN);
    m->dst_uuid[MAX_UUID_LEN] = 0;
    strncpy(m->src_uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    m->src_uuid[MAX_UUID_LEN] = 0;
    m->sdp_len = htons(static_cast<uint16_t>(plen));
    memcpy(m->sdp, local_sdp.data(), plen);
    send_proto(MSG_ICE_SDP, buf.data(), msglen);
    auto it = conns_.find(peer);
    if (it != conns_.end() && it->second->punch.have_lan) {
        send_proto(MSG_ICE_SDP, buf.data(), msglen, it->second->punch.direct_lan);
    }
    if (it != conns_.end()) {
        it->second->punch.ice_sdp_rtx_ms = plat_now_ms();
    }
    fprintf(stderr, "[P2PClient] ICE_SDP sent to %s len=%zu\n", peer.c_str(), plen);
}

void P2PClient::on_ice_sdp(const uint8_t* p, size_t plen) {
    if (!p || plen < (sizeof(IceSdpMsg) - 1)) return;
    const IceSdpMsg* m = reinterpret_cast<const IceSdpMsg*>(p);
    std::string dst(m->dst_uuid, strnlen(m->dst_uuid, MAX_UUID_LEN));
    std::string src(m->src_uuid, strnlen(m->src_uuid, MAX_UUID_LEN));
    uint16_t slen = ntohs(m->sdp_len);
    if (plen < (sizeof(IceSdpMsg) - 1 + slen)) return;
    std::string remote_sdp(m->sdp, slen);
    if (!dst.empty() && dst != cfg_.uuid) {
        fprintf(stderr, "[P2PClient] ICE_SDP dst=%s not self, drop\n", dst.c_str());
        return;
    }

    Conn* target = nullptr;
    if (!src.empty()) target = conn_of(src);
    if (!target) {
        for (auto& kv : conns_) {
            Conn& c = *kv.second;
            if (c.connecting() && c.punch.juice && c.punch.remote_sdp.empty()) {
                target = &c;
                break;
            }
        }
    }
    if (!target || !target->punch.juice) {
        const std::string key = !src.empty() ? src : std::string("_");
        pending_remote_sdp_[key] = remote_sdp;
        fprintf(stderr, "[P2PClient] ICE_SDP buffered from %s (invite not ready)\n",
                key.c_str());
        return;
    }
    apply_remote_ice_sdp(*target, remote_sdp);
}


void P2PClient::on_juice_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr) {
    auto* c = static_cast<Conn*>(user_ptr);
    if (!c || !data || size == 0) return;
    P2PClient* self = c->self;
    if (!self) return;
    std::lock_guard<std::recursive_mutex> lk(self->mu_);
    if (c->self != self) return;
    if (agent != c->punch.juice && agent != c->punch.juice_prev) return;
    // 对端已能把应用数据打过来：视为直连就绪（controlling 偶发不打 CONNECTED）
    if (agent == c->punch.juice && !c->punch.direct_ok) {
        c->punch.direct_ok = true;
        // 不在 recv 里 reap juice_prev：新 agent 可能尚未 nominated，媒体仍走旧路径
        self->set_connected(*c, false);
    }
    // libjuice 已解 ICE，data 为应用负载，直接送入隧道帧处理
    self->on_tunnel_frame(reinterpret_cast<const uint8_t*>(data), size, c->peer_uuid);
}

void P2PClient::do_punch(Conn& c) {
    if (c.punch.juice) return; // #19 ICE 由 libjuice 线程驱动，跳过自研发包
    ensure_punch_pool(c);
    auto* s = session_for(c.peer_uuid);
    std::vector<uint8_t> frame(14);
    codec_write_tunnel(frame.data(), (int)frame.size(), TT_PING, s->id(),
                       0, 0, 0, 0, nullptr, 0);
    // #17 多 socket 打洞池：每个 socket 均向目标发送，提升穿透概率
    if (c.punch.punch_socks.empty()) {
        sock_.send_to(frame.data(), frame.size(), c.punch.direct);
        if (c.punch.have_lan) sock_.send_to(frame.data(), frame.size(), c.punch.direct_lan);
        return;
    }
    for (auto& ps : c.punch.punch_socks) {
        ps.send_to(frame.data(), frame.size(), c.punch.direct);
        if (c.punch.have_lan) ps.send_to(frame.data(), frame.size(), c.punch.direct_lan);
    }
}

// ---------------------------------------------------------------------------
// 局域网组播发现（对标 TUTK LAN Search）
// ---------------------------------------------------------------------------
void P2PClient::tick_lan(uint64_t now) {
    if (!lan_enabled_) return;
    if (now >= next_lan_announce_ms_) {
        send_lan_beacon(MSG_LAN_ANNOUNCE, cfg_.uuid, nullptr);
        next_lan_announce_ms_ = now + 2000;
        for (auto it = lan_cache_.begin(); it != lan_cache_.end();) {
            if (now - it->second.seen_ms > 15000) it = lan_cache_.erase(it);
            else ++it;
        }
    }
}

void P2PClient::tick_ice(uint64_t now) {
    // 首包 host SDP 后延迟标记 gathering done：回环无 STUN，400ms 足够；
    // 公网仍等后续完整 SDP（见 apply_remote_ice_sdp），此处用 25s 兜底以免永远不收口。
    const uint64_t wait_ms = is_loopback_host(nat_server_.ip) ? 400 : 25000;
    for (auto& kv : conns_) {
        Conn& c = *kv.second;
        if (!c.punch.juice) continue;
        if (!c.connecting() && !c.punch.ice_restarting &&
            !(c.state == ConnState::Connected && c.punch.ice_sdp_rtx_n < 8))
            continue;
        // 完整 SDP 重传：对端若先 CONNECTED 会停发，本端丢包会永远 connecting。
        // 两侧都重传到满 8 次（约 1.6s），不因本端已直连而停。
        if (!c.punch.local_sdp.empty() &&
            c.punch.ice_sdp_rtx_n < 8 &&
            (c.punch.ice_sdp_rtx_ms == 0 || now - c.punch.ice_sdp_rtx_ms >= 200)) {
            send_ice_sdp(c.peer_uuid, c.punch.local_sdp);
            c.punch.ice_sdp_rtx_ms = now;
            c.punch.ice_sdp_rtx_n++;
        }
        if (c.punch.remote_sdp.empty() || c.punch.ice_remote_gather_done) continue;
        if (c.punch.ice_remote_applied_ms == 0) continue;
        if (now - c.punch.ice_remote_applied_ms < wait_ms) continue;
        juice_set_remote_gathering_done(c.punch.juice);
        c.punch.ice_remote_gather_done = true;
    }
}

void P2PClient::send_lan_beacon(uint8_t msg_id, const std::string& uuid,
                                const sockaddr_in* to) {
    if (!lan_enabled_ || lan_sock_.fd() < 0) return;
    LanBeacon b{};
    strncpy(b.uuid, uuid.c_str(), MAX_UUID_LEN);
    strncpy(b.src_uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    b.lan_port = htons(sock_.local_port());
    uint8_t buf[MAX_PKT];
    if (sizeof(LanBeacon) + 8 > sizeof(buf)) return;
    codec_write_head(buf, msg_id, (uint32_t)sizeof(LanBeacon));
    memcpy(buf + 8, &b, sizeof(b));
    if (to) {
        lan_sock_.send_to(buf, 8 + sizeof(b), *to);
        return;
    }
    sockaddr_in mcast{};
    if (sockaddr_from(LAN_MCAST_IP, LAN_MCAST_PORT, mcast))
        lan_sock_.send_to(buf, 8 + sizeof(b), mcast);
    sockaddr_in loop{};
    if (sockaddr_from("127.0.0.1", LAN_MCAST_PORT, loop))
        lan_sock_.send_to(buf, 8 + sizeof(b), loop);
}

void P2PClient::on_lan_beacon(uint8_t msg_id, const uint8_t* p, size_t plen,
                              const sockaddr_in& from) {
    if (plen < sizeof(LanBeacon)) return;
    LanBeacon b{};
    memcpy(&b, p, sizeof(b));
    b.uuid[MAX_UUID_LEN] = 0;
    b.src_uuid[MAX_UUID_LEN] = 0;
    std::string uuid(b.uuid, strnlen(b.uuid, MAX_UUID_LEN));
    std::string src(b.src_uuid, strnlen(b.src_uuid, MAX_UUID_LEN));
    if (msg_id == MSG_LAN_QUERY) {
        if (!src.empty() && src != cfg_.uuid)
            remember_lan_peer(src, from, b.lan_port);
        if (uuid.empty() || uuid == cfg_.uuid)
            send_lan_beacon(MSG_LAN_ANNOUNCE, cfg_.uuid, &from);
        return;
    }
    if (uuid.empty() || uuid == cfg_.uuid) return;
    remember_lan_peer(uuid, from, b.lan_port);
}

void P2PClient::remember_lan_peer(const std::string& uuid, const sockaddr_in& from,
                                  uint16_t media_port_nbo) {
    if (uuid.empty() || uuid == cfg_.uuid) return;
    LanCacheEnt ent;
    ent.addr = from;
    ent.addr.sin_port = media_port_nbo;
    ent.seen_ms = plat_now_ms();
    auto prev = lan_cache_.find(uuid);
    const bool first = (prev == lan_cache_.end() ||
                        prev->second.addr.sin_addr.s_addr != ent.addr.sin_addr.s_addr ||
                        prev->second.addr.sin_port != ent.addr.sin_port);
    lan_cache_[uuid] = ent;
    if (first) {
        fprintf(stderr, "[P2PClient] LAN found %s at %s:%u\n",
                uuid.c_str(), sockaddr_ip(ent.addr).c_str(),
                (unsigned)ntohs(media_port_nbo));
    }
    auto it = conns_.find(uuid);
    if (it != conns_.end() && it->second->state != ConnState::Idle)
        apply_lan_peer(uuid, from, media_port_nbo);
}

void P2PClient::apply_lan_peer(const std::string& uuid, const sockaddr_in& from,
                               uint16_t media_port_nbo) {
    auto it = conns_.find(uuid);
    if (it == conns_.end()) return;
    Conn& c = *it->second;
    c.punch.direct_lan = from;
    c.punch.direct_lan.sin_port = media_port_nbo;
    c.punch.have_lan = true;
    addr_to_peer_[addr_key(c.punch.direct_lan)] = uuid;
}

// ---------------------------------------------------------------------------
// 中继
// ---------------------------------------------------------------------------
void P2PClient::note_proxy_tcp(const std::string& ip, uint16_t port) {
    if (ip.empty() || port == 0) return;
    if (derp_addr_.ip.empty() || derp_addr_.port == 0) {
        derp_addr_.ip = ip;
        derp_addr_.port = port;
    }
    // 只记地址，建链放到 tick_relay，避免在 CONNECT/ICE 回调里阻塞
}

bool P2PClient::derp_try_connect() {
    if (derp_fd_.valid() || derp_addr_.port == 0 || derp_addr_.ip.empty())
        return derp_fd_.valid();
    TcpFd fd;
    if (!fd.open()) {
        printf("[P2PClient] DERP tcp socket open failed\n");
        fflush(stdout);
        return false;
    }
    fd.set_nodelay();
    fd.set_timeout_ms(400);
    if (!fd.connect_to(derp_addr_.ip.c_str(), derp_addr_.port)) {
        printf("[P2PClient] DERP tcp connect %s:%u failed errno=%d\n",
               derp_addr_.ip.c_str(), (unsigned)derp_addr_.port, errno);
        fflush(stdout);
        return false;
    }
    if (cfg_.proxy_tcp_tls) {
        if (!tls_make_client_ctx(derp_tls_ctx_, cfg_.proxy_tls_insecure) ||
            !derp_tls_.connect(derp_tls_ctx_.ctx, fd.fd())) {
            printf("[P2PClient] DERP tls handshake failed\n");
            fflush(stdout);
            return false;
        }
        derp_use_tls_ = true;
    } else {
        derp_use_tls_ = false;
    }
    fd.set_nonblock();
    derp_fd_ = std::move(fd);
    derp_rbuf_.clear();
    printf("[P2PClient] DERP tcp ready %s:%u tls=%d\n",
           derp_addr_.ip.c_str(), (unsigned)derp_addr_.port, derp_use_tls_ ? 1 : 0);
    fflush(stdout);

    static const uint8_t kProxyAuthKey[] = "p2p-proxy-auth-2024";
    ProxyRegReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    hmac_sha256(kProxyAuthKey, sizeof(kProxyAuthKey) - 1,
                reinterpret_cast<const uint8_t*>(cfg_.uuid.data()), cfg_.uuid.size(),
                req.hmac);
    derp_send(MSG_PROXY_REGISTER_REQ, &req, sizeof(req));
    return true;
}

void P2PClient::derp_close() {
    derp_registered_.store(false);
    derp_tls_.close();
    derp_fd_.close();
    derp_rbuf_.clear();
    next_derp_try_ms_ = plat_now_ms() + derp_backoff_ms_;
    if (derp_backoff_ms_ < 8000) derp_backoff_ms_ *= 2;
}

bool P2PClient::derp_send(uint8_t msg_id, const void* payload, size_t plen) {
    if (!derp_fd_.valid()) return false;
    uint8_t buf[MAX_PKT];
    const size_t total = build_msg(buf, sizeof(buf), msg_id, payload, plen);
    if (total == 0) return false;
    if (derp_use_tls_) return derp_tls_.write(buf, total) == (ssize_t)total;
    return derp_fd_.send_all(buf, total) == (ssize_t)total;
}

void P2PClient::derp_on_readable() {
    if (!derp_fd_.valid()) return;
    uint8_t tmp[MAX_PKT];
    ssize_t n = 0;
    if (derp_use_tls_) n = derp_tls_.read(tmp, sizeof(tmp));
    else n = derp_fd_.recv_some(tmp, sizeof(tmp));
    if (n < 0) {
        fprintf(stderr, "[P2PClient] DERP tcp closed\n");
        derp_close();
        return;
    }
    if (n == 0) return;
    derp_rbuf_.insert(derp_rbuf_.end(), tmp, tmp + n);
    for (;;) {
        uint8_t msg_id = 0;
        std::vector<uint8_t> payload;
        int c = xn_pop_frame(derp_rbuf_, msg_id, payload);
        if (c == 0) break;
        if (c < 0) { derp_close(); break; }
        if (msg_id == MSG_PROXY_REGISTER_RSP) {
            on_proxy_register_rsp(payload.data(), payload.size());
            if (!payload.empty() && payload[0] == 0) {
                derp_registered_.store(true);
                derp_backoff_ms_ = 400;
                printf("[P2PClient] DERP tcp registered\n");
                fflush(stdout);
            }
        } else if (msg_id == MSG_PROXY_RELAY_DATA) {
            on_proxy_relay_data(payload.data(), payload.size());
        }
    }
}

void P2PClient::relay_register() {
    if (proxies_.empty()) return;
    // 与 proxy 端共享的注册鉴权密钥（HMAC-SHA256(key, uuid)，防伪造注册）
    static const uint8_t kProxyAuthKey[] = "p2p-proxy-auth-2024";
    ProxyRegReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
    hmac_sha256(kProxyAuthKey, sizeof(kProxyAuthKey) - 1,
                reinterpret_cast<const uint8_t*>(cfg_.uuid.data()), cfg_.uuid.size(),
                req.hmac);
    for (const auto& px : proxies_) {
        sockaddr_in to;
        if (sockaddr_from(px.ip, px.port, to))
            send_proto(MSG_PROXY_REGISTER_REQ, &req, sizeof(req), to);
    }
}

void P2PClient::send_tunnel_via(Conn& c, const uint8_t* frame, size_t len) {
    uint64_t now = plat_now_ms();
    bool direct_proven = c.punch.direct_ok && c.punch.have_direct;
    bool relay_ready = c.relay.relay_ok || derp_registered_.load() ||
                       (relay_registered_ && !proxies_.empty() &&
                        (cfg_.force_relay || now >= c.punch.punch_deadline));

    // 鉴权开启时对隧道负载做流加密（iv[8]+cipher，负载>0 才加密）
    uint8_t encbuf[MAX_PKT];
    const uint8_t* tx = frame;
    size_t txlen = len;
    if (encrypt_tunnel_frame(c.peer_uuid, encbuf, frame, len, &txlen))
        tx = encbuf;

    // 当前 agent 已 nominated 才 juice_send；restart 换代期间走 juice_prev
    if (c.punch.juice && c.punch.ice_nominated) {
        juice_send(c.punch.juice, reinterpret_cast<const char*>(tx), txlen);
        return;
    }
    if (c.punch.juice_prev) {
        juice_send(c.punch.juice_prev, reinterpret_cast<const char*>(tx), txlen);
        return;
    }
    if (c.punch.juice && c.punch.direct_ok) {
        juice_send(c.punch.juice, reinterpret_cast<const char*>(tx), txlen);
        return;
    }

    if (direct_proven || (!relay_ready && c.punch.have_direct)) {
        // 直连已打通 / 打洞窗口内：优先使用确认直连的打洞 socket 发送
        if (c.punch.direct_sock_idx >= 0 &&
            (size_t)c.punch.direct_sock_idx < c.punch.punch_socks.size()) {
            auto& ps = c.punch.punch_socks[c.punch.direct_sock_idx];
            ps.send_to(tx, txlen, c.punch.direct);
            if (c.punch.have_lan) ps.send_to(tx, txlen, c.punch.direct_lan);
        } else {
            sock_.send_to(tx, txlen, c.punch.direct);
            if (c.punch.have_lan) sock_.send_to(tx, txlen, c.punch.direct_lan);
        }
        return;
    }
    if (relay_ready && (derp_registered_.load() ||
                        (relay_registered_ && !proxies_.empty()))) {
        uint8_t body[MAX_PKT];
        if (sizeof(RelayFrame) + txlen > sizeof(body)) return;
        RelayFrame rf{};
        strncpy(rf.src_uuid, cfg_.uuid.c_str(), MAX_UUID_LEN);
        strncpy(rf.dst_uuid, c.peer_uuid.c_str(), MAX_UUID_LEN);
        memcpy(body, &rf, sizeof(rf));
        memcpy(body + sizeof(rf), tx, txlen);
        const size_t blen = sizeof(rf) + txlen;
        if (derp_registered_.load() && derp_send(MSG_PROXY_RELAY_DATA, body, blen))
            return;
        if (relay_registered_ && !proxies_.empty()) {
            uint8_t buf[MAX_PKT];
            size_t need = 8 + blen;
            if (need > sizeof(buf)) return;
            codec_write_head(buf, MSG_PROXY_RELAY_DATA, (uint32_t)blen);
            memcpy(buf + 8, body, blen);
            sockaddr_in to;
            if (sockaddr_from(proxies_[0].ip, proxies_[0].port, to))
                sock_.send_to(buf, need, to);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// 隧道负载加密
// ---------------------------------------------------------------------------
bool P2PClient::psk_tunnel_key(const std::string& peer, uint8_t out[32]) {
    if (secret_.empty() || peer.empty() || peer == cfg_.uuid) return false;
    auto it = tunnel_keys_.find(peer);
    if (it != tunnel_keys_.end()) {
        memcpy(out, it->second.data(), 32);
        return true;
    }
    std::string a = cfg_.uuid < peer ? cfg_.uuid : peer;
    std::string b = cfg_.uuid < peer ? peer : cfg_.uuid;
    std::string msg = std::string(TUNNEL_KEY_PREFIX) + a + ":" + b;
    uint8_t key[32];
    hmac_sha256((const uint8_t*)secret_.data(), secret_.size(),
                (const uint8_t*)msg.data(), msg.size(), key);
    std::array<uint8_t, 32> arr;
    memcpy(arr.data(), key, 32);
    tunnel_keys_[peer] = arr;
    memcpy(out, key, 32);
    return true;
}

bool P2PClient::get_tunnel_key(const std::string& peer, uint8_t out[32]) {
    auto it = fs_keys_.find(peer);
    auto hit = hs_.find(peer);
    // 仅在对端已 ACK（确认已派生）后改用 FS 加密，避免对端尚未完成握手时解不开
    if (it != fs_keys_.end() && it->second.has_cur &&
        hit != hs_.end() && hit->second.peer_acked) {
        memcpy(out, it->second.cur.data(), 32);
        return true;
    }
    return psk_tunnel_key(peer, out);
}

bool P2PClient::encrypt_tunnel_frame(const std::string& peer, uint8_t* out,
                                     const uint8_t* in, size_t inlen, size_t* outlen) {
    if (inlen < 14) return false;
    uint16_t plen = (uint16_t)((in[12] << 8) | in[13]);
    if (plen == 0 || 14 + 12 + plen + 16 > MAX_PKT) return false;
    uint8_t key[32];
    if (!get_tunnel_key(peer, key)) return false;

    memcpy(out, in, 14);                       // 帧头保持明文（type/seq/ack 元数据）
    uint8_t nonce[12];
    if (p2p_random_bytes(nonce, sizeof(nonce)) != 0) return false;
    memcpy(out + 14, nonce, 12);

    // AEAD 加密：AES-256-CTR + HMAC-SHA256
    size_t cipher_len = 0;
    uint8_t tag[16];
    if (p2p_aead_encrypt(key, nonce, in + 14, plen, out + 14 + 12, plen, &cipher_len, tag) != 0) {
        return false;
    }
    memcpy(out + 14 + 12 + cipher_len, tag, 16);

    out[7] |= TF_ENC;                          // flags
    uint16_t nl = (uint16_t)(12 + cipher_len + 16);
    out[12] = (uint8_t)(nl >> 8);
    out[13] = (uint8_t)(nl & 0xFF);
    *outlen = 14 + 12 + cipher_len + 16;
    tunnel_enc_tx_.fetch_add(1);
    return true;
}

bool P2PClient::decrypt_tunnel_frame(const std::string& peer, uint8_t* out,
                                     const uint8_t* in, size_t inlen, size_t* outlen) {
    if (inlen < 14 || !(in[7] & TF_ENC)) return false;
    uint16_t plen = (uint16_t)((in[12] << 8) | in[13]);
    if (plen < 12 + 16 || 14 + plen - 12 - 16 > MAX_PKT) return false;

    uint8_t keys[3][32];
    int nkeys = 0;
    auto it = fs_keys_.find(peer);
    if (it != fs_keys_.end()) {
        if (it->second.has_cur) {
            memcpy(keys[nkeys++], it->second.cur.data(), 32);
        }
        if (it->second.has_prev) {
            memcpy(keys[nkeys++], it->second.prev.data(), 32);
        }
    }
    if (nkeys < 3 && psk_tunnel_key(peer, keys[nkeys])) nkeys++;
    if (nkeys == 0) return false;

    memcpy(out, in, 14);
    const uint8_t* nonce = in + 14;
    size_t cipher_len = plen - 12 - 16;
    const uint8_t* tag = in + 14 + 12 + cipher_len;

    size_t plain_len = 0;
    bool ok = false;
    for (int i = 0; i < nkeys; i++) {
        if (p2p_aead_decrypt(keys[i], nonce, in + 14 + 12, cipher_len,
                             out + 14, cipher_len, &plain_len, tag) == 0) {
            ok = true;
            break;
        }
    }
    if (!ok) return false;

    out[7] &= (uint8_t)~TF_ENC;
    uint16_t nl = (uint16_t)(plain_len);
    out[12] = (uint8_t)(nl >> 8);
    out[13] = (uint8_t)(nl & 0xFF);
    *outlen = 14 + plain_len;
    tunnel_enc_rx_.fetch_add(1);
    return true;
}

// ---------------------------------------------------------------------------
// 会话
// ---------------------------------------------------------------------------
Session* P2PClient::session_for(const std::string& peer) {
    auto it = sessions_.find(peer);
    if (it != sessions_.end()) return it->second.get();
    auto s = std::make_unique<Session>(next_session_id_++);
    s->on_data = [this, peer](uint8_t ch, const uint8_t* d, size_t n) {
        if (ch == HS_CHANNEL) {
            on_hs_msg(peer, d, n);
            return;
        }
        if (on_message) on_message(peer, ch, d, n);
    };
    s->on_tx = [this, peer](const uint8_t* f, size_t n) {
        auto it2 = conns_.find(peer);
        if (it2 != conns_.end()) send_tunnel_via(*it2->second, f, n);
    };
    Session* raw = s.get();
    sessions_.emplace(peer, std::move(s));
    return raw;
}

void P2PClient::on_tunnel_frame(const uint8_t* frame, size_t len,
                                const std::string& peer_uuid) {
    uint8_t decbuf[MAX_PKT];
    const uint8_t* f = frame;
    size_t fl = len;
    if (decrypt_tunnel_frame(peer_uuid, decbuf, frame, len, &fl))
        f = decbuf;
    if (auto* s = session_for(peer_uuid)) s->on_frame(f, fl);
}

void P2PClient::close_conn(Conn& c) {
    const std::string peer = c.peer_uuid;
    for (auto it = addr_to_peer_.begin(); it != addr_to_peer_.end();) {
        if (it->second == peer) it = addr_to_peer_.erase(it);
        else ++it;
    }
    // #17 关闭打洞 socket 池
    for (auto& ps : c.punch.punch_socks) ps.close();
    c.punch.punch_socks.clear();
    c.punch.direct_sock_idx = -1;
    // 摘下 juice，锁外销毁：juice_destroy 会等回调结束，回调自身要拿 mu_
    if (c.punch.juice) {
        juice_reap_.push_back(c.punch.juice);
        c.punch.juice = nullptr;
    }
    if (c.punch.juice_prev) {
        juice_reap_.push_back(c.punch.juice_prev);
        c.punch.juice_prev = nullptr;
    }
    c.self = nullptr;  // 使在途 ICE 回调能检测失效
    {
        auto hit = hs_.find(peer);
        if (hit != hs_.end()) {
            memset(hit->second.priv, 0, sizeof(hit->second.priv));
            hs_.erase(hit);
        }
        fs_keys_.erase(peer);
    }
    pending_remote_sdp_.erase(peer);
    sessions_.erase(peer);
    conns_.erase(peer);
    if (on_disconnected) on_disconnected(peer);
}

} // namespace p2p
