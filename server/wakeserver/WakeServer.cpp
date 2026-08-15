// WakeServer：低功耗设备 UDP 保活 / 唤醒（P7）
//   ./p2p_wakeserver <Port> [WakeSecret]
// 协议：MSG_WAKE_KEEPALIVE / MSG_WAKE_TRIGGER / MSG_WAKE_RESULT / MSG_WAKE_POKE
#include "WakeServer.h"
#include "Crypto.h"
#include "Log.h"
#include "Packet.h"
#include "Util.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/select.h>
#include <unistd.h>

using namespace p2p;

static WakeServer* g_wake = nullptr;
static void on_signal(int) { if (g_wake) g_wake->request_stop(); }

int WakeServer::init(uint16_t port, const std::string& secret, uint32_t ttl_sec) {
    port_ = port;
    secret_ = secret;
    ttl_sec_ = ttl_sec ? ttl_sec : 180;
    if (!sock_.open() || !sock_.set_reuse(false) || !sock_.bind_any(port_)) {
        perror("[WakeServer] bind");
        return -1;
    }
    LOGI("WakeServer", "listening on port %u ttl=%us auth=%d",
         (unsigned)port_, ttl_sec_, secret_.empty() ? 0 : 1);
    return 0;
}

int WakeServer::run() {
    running_ = true;
    uint8_t buf[MAX_PKT];
    time_t last_sweep = time(nullptr);
    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_.fd(), &rfds);
        timeval tv{1, 0};
        int n = select(sock_.fd() + 1, &rfds, nullptr, nullptr, &tv);
        if (!running_) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n > 0 && FD_ISSET(sock_.fd(), &rfds)) {
            sockaddr_in from{};
            ssize_t r = sock_.recv_from(buf, sizeof(buf), from);
            if (r > 0) handle(buf, (size_t)r, from);
        }
        time_t now = time(nullptr);
        if (now - last_sweep >= 10) {
            sweep_expired();
            last_sweep = now;
        }
    }
    return 0;
}

void WakeServer::handle(const uint8_t* data, size_t len, const sockaddr_in& from) {
    PacketReader r(data, len);
    MsgHead h{};
    if (!r.read_struct(h)) return;
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return;
    const uint32_t body_len = ntohl(h.length);
    if (body_len > r.remaining()) return;
    const uint8_t* p = data + sizeof(MsgHead);
    switch (h.msg_id) {
    case MSG_WAKE_KEEPALIVE: on_keepalive(p, body_len, from); break;
    case MSG_WAKE_TRIGGER:   on_trigger(p, body_len, from); break;
    default: break;
    }
}

bool WakeServer::verify_hmac(const char* uuid, const uint8_t mac[32]) const {
    if (secret_.empty()) return true;
    uint8_t expect[32];
    hmac_sha256(reinterpret_cast<const uint8_t*>(secret_.data()), secret_.size(),
                reinterpret_cast<const uint8_t*>(uuid), strnlen(uuid, MAX_UUID_LEN),
                expect);
    return p2p_const_time_eq(expect, mac, 32);
}

void WakeServer::on_keepalive(const uint8_t* p, size_t plen, const sockaddr_in& from) {
    if (plen < sizeof(WakeKeepalive)) return;
    WakeKeepalive req{};
    memcpy(&req, p, sizeof(req));
    req.uuid[MAX_UUID_LEN] = 0;
    if (!req.uuid[0]) return;
    if (!verify_hmac(req.uuid, req.hmac)) {
        send_result(from, WAKE_BAD_MAC, req.uuid);
        LOGW("WakeServer", "KEEP uuid[%s] bad mac from %s",
             req.uuid, addr_to_str(from).c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        Entry e;
        e.addr = from;
        e.last_seen = time(nullptr);
        table_[req.uuid] = e;
    }
    send_result(from, WAKE_OK, req.uuid);
    LOGI("WakeServer", "KEEP uuid[%s] from %s", req.uuid, addr_to_str(from).c_str());
}

void WakeServer::on_trigger(const uint8_t* p, size_t plen, const sockaddr_in& from) {
    if (plen < sizeof(WakeTrigger)) return;
    WakeTrigger req{};
    memcpy(&req, p, sizeof(req));
    req.uuid[MAX_UUID_LEN] = 0;
    if (!req.uuid[0]) return;

    sockaddr_in dest{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = table_.find(req.uuid);
        if (it != table_.end()) {
            dest = it->second.addr;
            found = true;
        }
    }
    if (!found) {
        send_result(from, WAKE_NOT_FOUND, req.uuid);
        LOGI("WakeServer", "TRIGGER uuid[%s] not found", req.uuid);
        return;
    }
    send_poke(dest, req.uuid);
    send_result(from, WAKE_OK, req.uuid);
    LOGI("WakeServer", "TRIGGER uuid[%s] poke -> %s",
         req.uuid, addr_to_str(dest).c_str());
}

void WakeServer::send_result(const sockaddr_in& to, uint8_t result, const char* uuid) {
    WakeResult rsp{};
    rsp.result = result;
    copy_str_field(rsp.uuid, sizeof(rsp.uuid), uuid);
    uint8_t buf[MAX_PKT];
    size_t n = build_msg(buf, sizeof(buf), MSG_WAKE_RESULT, &rsp, sizeof(rsp));
    if (n) sock_.send_to(buf, n, to);
}

void WakeServer::send_poke(const sockaddr_in& to, const char* uuid) {
    WakeResult poke{};
    poke.result = WAKE_OK;
    copy_str_field(poke.uuid, sizeof(poke.uuid), uuid);
    uint8_t buf[MAX_PKT];
    size_t n = build_msg(buf, sizeof(buf), MSG_WAKE_POKE, &poke, sizeof(poke));
    if (n) sock_.send_to(buf, n, to);
}

void WakeServer::sweep_expired() {
    const time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = table_.begin(); it != table_.end(); ) {
        if (now - it->second.last_seen > (time_t)ttl_sec_) {
            LOGI("WakeServer", "expire uuid[%s]", it->first.c_str());
            it = table_.erase(it);
        } else {
            ++it;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <Port> [WakeSecret]\n", argv[0]);
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    std::string secret = (argc >= 3) ? argv[2] : "";
    WakeServer srv;
    if (srv.init(port, secret) != 0) return 1;
    g_wake = &srv;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    return srv.run();
}
