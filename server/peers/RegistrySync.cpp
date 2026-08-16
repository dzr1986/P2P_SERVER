#include "server/peers/RegistrySync.h"
#include "core/foundation/Crypto.h"
#include "core/foundation/Log.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace p2p {

void RegistrySync::bind(SendFn send, PeerManager* peers) {
    send_ = std::move(send);
    peers_ = peers;
}

void RegistrySync::setup(const std::string& wan_ip, uint16_t nat_port,
                         const LocalListeners& listeners) {
    addrs_.clear();
    if (!cfg_ || !cfg_->sync_enabled) { enabled_ = false; return; }
    enabled_ = true;

    std::vector<std::string> list = cfg_->sync_addrs;
    if (list.empty()) list = cfg_->nat_ips;

    std::string self_ip = wan_ip;
    if (self_ip == "0.0.0.0") self_ip = "127.0.0.1";

    for (auto& s : list) {
        std::string ip = s;
        uint16_t port = nat_port;
        size_t pos = ip.find(':');
        if (pos != std::string::npos) {
            port = (uint16_t)atoi(ip.c_str() + pos + 1);
            ip = ip.substr(0, pos);
        }
        if (ip == self_ip && port == nat_port) continue;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) continue;
        if (listeners.is_local(a)) continue;
        addrs_.push_back(a);
        LOGI("RegistrySync", "peer [%s:%d]", ip.c_str(), port);
    }
    LOGI("RegistrySync", "registry sync enabled, %zu peer(s)", addrs_.size());
}

void RegistrySync::send_signed(const sockaddr_in& to, uint8_t msg_id,
                               const uint8_t* payload, size_t plen) {
    if (!send_ || !cfg_) return;
    uint8_t buf[MAX_PKT];
    if (plen > sizeof(buf) - 32) return;
    memcpy(buf, payload, plen);
    if (!cfg_->sync_auth_secret.empty()) {
        uint8_t mac[32];
        hmac_sha256((const uint8_t*)cfg_->sync_auth_secret.data(),
                    cfg_->sync_auth_secret.size(), payload, plen, mac);
        memcpy(buf + plen, mac, 32);
        plen += 32;
    }
    send_(to, msg_id, buf, plen);
}

bool RegistrySync::verify(const uint8_t* payload, size_t plen, size_t body_len) const {
    if (!cfg_ || cfg_->sync_auth_secret.empty()) return true;
    if (body_len + 32 > plen) return false;
    uint8_t expect[32];
    hmac_sha256((const uint8_t*)cfg_->sync_auth_secret.data(),
                cfg_->sync_auth_secret.size(), payload, body_len, expect);
    return p2p_const_time_eq(expect, payload + body_len, 32);
}

void RegistrySync::broadcast(uint8_t msg_id, const void* payload, size_t plen,
                             const sockaddr_in* except) {
    for (auto& to : addrs_) {
        if (except && except->sin_addr.s_addr == to.sin_addr.s_addr &&
            except->sin_port == to.sin_port)
            continue;
        send_signed(to, msg_id, (const uint8_t*)payload, plen);
    }
}

bool RegistrySync::seen(const std::string& uuid, uint32_t hb_time,
                        const sockaddr_in& from) {
    char key[160];
    snprintf(key, sizeof(key), "%s|%u|%u:%u", uuid.c_str(), hb_time,
             ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = seen_.find(key);
    if (it != seen_.end() && now - it->second < 300) return true;
    seen_[key] = now;
    if (seen_.size() > 16384) {
        for (auto jt = seen_.begin(); jt != seen_.end();) {
            if (now - jt->second >= 300) jt = seen_.erase(jt);
            else ++jt;
        }
    }
    return false;
}

void RegistrySync::send_peer_entry(const sockaddr_in& to, const Peer& p, uint8_t hop) {
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
    send_signed(to, MSG_SYNC_PEER_ENTRY, buf, total);
}

void RegistrySync::broadcast_peer(const UuidReq& req, const std::string& extinfo,
                                  const sockaddr_in& pub) {
    if (!enabled()) return;
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
    send_peer_entry(addrs_[0], p, 1);
    for (size_t i = 1; i < addrs_.size(); i++)
        send_peer_entry(addrs_[i], p, 1);
}

void RegistrySync::handle_entry(const SyncPeerEntry& e, const std::string& extinfo,
                                const sockaddr_in& from) {
    if (!enabled() || !peers_) return;
    uint8_t hop = e.hop;
    uint32_t hb_time = ntohl(e.hb_time);
    std::string uuid(e.uuid);
    if (uuid.empty()) return;
    if (hop != SYNC_HOP_SNAP && seen(uuid, hb_time, from)) {
        LOGI("RegistrySync", "dup skip uuid[%s] hb[%u]", uuid.c_str(), hb_time);
        return;
    }
    sockaddr_in pub{}, lan{};
    pub.sin_family = AF_INET;
    pub.sin_port = e.pub_port;
    inet_pton(AF_INET, e.pub_ip, &pub.sin_addr);
    lan = pub;
    lan.sin_port = e.lan_port;
    inet_pton(AF_INET, e.lan_ip, &lan.sin_addr);
    peers_->upsert_synced(uuid, pub, lan, e.dev_type, e.nattype, extinfo, (time_t)hb_time);
    LOGI("RegistrySync", "sync entry uuid[%s] hop[%d] hb[%u] pub[%s:%d] from [%s:%d]",
         uuid.c_str(), hop, hb_time, e.pub_ip, ntohs(e.pub_port),
         inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    if (hop != SYNC_HOP_SNAP && hop < SYNC_HOP_MAX) {
        SyncPeerEntry f = e;
        f.hop = hop + 1;
        broadcast(MSG_SYNC_PEER_ENTRY, &f, sizeof(f) + extinfo.size(), &from);
    }
}

void RegistrySync::handle_del(const std::string& uuid, const sockaddr_in& from) {
    if (!enabled() || !peers_) return;
    SyncPeerDel d{};
    strncpy(d.uuid, uuid.c_str(), MAX_UUID_LEN);
    broadcast(MSG_SYNC_PEER_DEL, &d, sizeof(d), &from);
    peers_->remove(uuid);
    LOGI("RegistrySync", "del uuid[%s]", uuid.c_str());
}

void RegistrySync::handle_snapshot_req(const sockaddr_in& from) {
    if (!enabled() || !peers_) return;
    auto snap = peers_->snapshot();
    for (auto& p : snap) send_peer_entry(from, p, SYNC_HOP_SNAP);
    LOGI("RegistrySync", "sync snapshot sent %zu entries to [%s:%d]", snap.size(),
         inet_ntoa(from.sin_addr), ntohs(from.sin_port));
}

void RegistrySync::request_snapshot() {
    if (!enabled()) return;
    SyncSnapshotReq req{};
    req.ts = htonl((uint32_t)time(nullptr));
    broadcast(MSG_SYNC_SNAPSHOT_REQ, &req, sizeof(req), nullptr);
}

} // namespace p2p
