// P2PTunnel：本地 TCP ↔ IOTC 可靠通道双向搬运
#include "P2PTunnelAPIs.h"

#include "IOTC.h"
#include "TunnelCodec.h"
#include "core/socket/Net.h"
#include "core/packet/ProtoDef.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace p2p;

struct TunSlot {
    bool used = false;
    std::atomic<bool> running{false};
    int sid = -1;
    uint8_t channel = 0;
    bool is_serve = false;
    std::string target_ip;
    uint16_t target_port = 0;
    uint16_t listen_port = 0;
    TcpFd listen;
    std::mutex mu;
    std::unordered_map<uint16_t, int> fds;   // conn_id -> raw fd（pump 线程接管生命周期）
    uint16_t next_id = 1;
    std::thread net_thr;
    std::thread acc_thr;
    std::vector<std::thread> pumps;
};

std::mutex g_mu;
TunSlot g_tun[TUNNEL_MAX_HANDLES];

int send_tun(int sid, uint8_t ch, uint8_t type, uint16_t cid,
             const uint8_t* payload, size_t plen) {
    uint8_t pkt[MAX_TUNNEL_PAYLOAD];
    const size_t n = tun_write(pkt, sizeof(pkt), type, cid, payload, plen);
    if (n == 0) return TUNNEL_ER_InvalidArg;
    const int r = IOTC_Session_Write(sid, ch, pkt, (int)n, 1);
    return r == IOTC_ER_NoERROR ? TUNNEL_ER_NoERROR : TUNNEL_ER_ConnectFail;
}

void close_conn(TunSlot& s, uint16_t cid) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.fds.find(cid);
        if (it == s.fds.end()) return;
        fd = it->second;
        s.fds.erase(it);
    }
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

void pump_tcp_to_iotc(TunSlot* s, uint16_t cid, int fd) {
    uint8_t buf[1024];
    while (s->running.load()) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (send_tun(s->sid, s->channel, TUN_DATA, cid, buf, (size_t)n) != TUNNEL_ER_NoERROR)
            break;
    }
    send_tun(s->sid, s->channel, TUN_CLOSE, cid, nullptr, 0);
    close_conn(*s, cid);
}

void spawn_pump(TunSlot& s, uint16_t cid, int fd) {
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.fds[cid] = fd;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    s.pumps.emplace_back(pump_tcp_to_iotc, &s, cid, fd);
}

void net_loop(TunSlot* s) {
    uint8_t msg[MAX_TUNNEL_PAYLOAD];
    while (s->running.load()) {
        const int n = IOTC_Session_Read(s->sid, s->channel, msg, sizeof(msg), 200);
        if (n == IOTC_ER_Timeout) continue;
        if (n <= 0) break;
        uint8_t type = 0;
        uint16_t cid = 0;
        const uint8_t* payload = nullptr;
        size_t plen = 0;
        if (!tun_read(msg, (size_t)n, type, cid, payload, plen)) continue;

        if (type == TUN_OPEN && s->is_serve) {
            TcpFd cli;
            if (!cli.open() || !cli.connect_to(s->target_ip.c_str(), s->target_port)) {
                send_tun(s->sid, s->channel, TUN_CLOSE, cid, nullptr, 0);
                continue;
            }
            const int fd = cli.release();
            send_tun(s->sid, s->channel, TUN_OPEN_ACK, cid, nullptr, 0);
            spawn_pump(*s, cid, fd);
        } else if (type == TUN_DATA) {
            int fd = -1;
            {
                std::lock_guard<std::mutex> lk(s->mu);
                auto it = s->fds.find(cid);
                if (it != s->fds.end()) fd = it->second;
            }
            if (fd >= 0 && plen > 0) ::send(fd, payload, plen, MSG_NOSIGNAL);
        } else if (type == TUN_CLOSE) {
            close_conn(*s, cid);
        }
        // OPEN_ACK：客户端 accept 后已开始 pump，无需额外动作
    }
}

void accept_loop(TunSlot* s) {
    while (s->running.load()) {
        pollfd pfd{};
        pfd.fd = s->listen.fd();
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        TcpFd cli = s->listen.accept_one();
        if (!cli.valid()) continue;
        const uint16_t cid = s->next_id++;
        if (send_tun(s->sid, s->channel, TUN_OPEN, cid, nullptr, 0) != TUNNEL_ER_NoERROR) {
            continue;
        }
        spawn_pump(*s, cid, cli.release());
    }
}

int alloc_handle() {
    for (int i = 0; i < TUNNEL_MAX_HANDLES; i++) {
        if (!g_tun[i].used) return i;
    }
    return -1;
}

void stop_locked(TunSlot& s) {
    s.running = false;
    s.listen.shutdown_rw();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        for (auto& kv : s.fds) {
            if (kv.second >= 0) {
                ::shutdown(kv.second, SHUT_RDWR);
                ::close(kv.second);
            }
        }
        s.fds.clear();
    }
    if (s.net_thr.joinable()) s.net_thr.join();
    if (s.acc_thr.joinable()) s.acc_thr.join();
    for (auto& t : s.pumps) {
        if (t.joinable()) t.join();
    }
    s.pumps.clear();
    s.listen.close();
    s.used = false;
}

} // namespace

extern "C" {

int P2PTunnel_Serve(int sid, uint8_t channel,
                    const char* target_ip, uint16_t target_port) {
    if (sid < 0 || !target_ip || target_port == 0 || channel >= IOTC_MAX_CHANNELS)
        return TUNNEL_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    const int h = alloc_handle();
    if (h < 0) return TUNNEL_ER_ExceedMax;
    auto& s = g_tun[h];
    s.used = true;
    s.running.store(true);
    s.sid = sid;
    s.channel = channel;
    s.is_serve = true;
    s.target_ip = target_ip;
    s.target_port = target_port;
    s.listen_port = 0;
    s.next_id = 1;
    s.net_thr = std::thread(net_loop, &s);
    return h;
}

int P2PTunnel_Map(int sid, uint8_t channel, uint16_t local_port) {
    if (sid < 0 || channel >= IOTC_MAX_CHANNELS) return TUNNEL_ER_InvalidArg;
    TcpFd listen;
    if (!listen.open() || !listen.set_reuse() || !listen.bind_loopback(local_port) ||
        !listen.listen()) {
        return TUNNEL_ER_ListenFail;
    }
    const uint16_t bound = listen.local_port();
    std::lock_guard<std::mutex> lk(g_mu);
    const int h = alloc_handle();
    if (h < 0) return TUNNEL_ER_ExceedMax;
    auto& s = g_tun[h];
    s.used = true;
    s.running.store(true);
    s.sid = sid;
    s.channel = channel;
    s.is_serve = false;
    s.listen_port = bound;
    s.next_id = 1;
    s.listen = std::move(listen);
    s.net_thr = std::thread(net_loop, &s);
    s.acc_thr = std::thread(accept_loop, &s);
    return h;
}

int P2PTunnel_LocalPort(int handle, uint16_t* port) {
    if (!port) return TUNNEL_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    if (handle < 0 || handle >= TUNNEL_MAX_HANDLES || !g_tun[handle].used)
        return TUNNEL_ER_HandleNoExist;
    *port = g_tun[handle].listen_port;
    return TUNNEL_ER_NoERROR;
}

void P2PTunnel_Stop(int handle) {
    if (handle < 0 || handle >= TUNNEL_MAX_HANDLES) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_tun[handle].used) return;
    stop_locked(g_tun[handle]);
}

} // extern "C"
