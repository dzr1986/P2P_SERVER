#include "PeerManage.h"
#include "ProtoDef.h"

#include <cstring>

namespace p2p {

PeerManager::PeerManager() {}
PeerManager::~PeerManager() {}

bool PeerManager::upsert(const std::string& uuid, const sockaddr_in& pub,
                         const sockaddr_in& lan, uint8_t dev_type, uint8_t nattype,
                         const std::string& extinfo) {
    if (uuid.empty() || uuid.size() > MAX_UUID_LEN) return false;

    std::lock_guard<std::mutex> lk(mu_);
    time_t now = time(nullptr);
    auto it = peers_.find(uuid);
    if (it == peers_.end()) {
        Peer p;
        p.uuid = uuid;
        p.pub_addr = pub;
        p.lan_addr = lan;
        p.dev_type = dev_type;
        p.nattype = nattype;
        p.last_heartbeat = now;
        p.register_time = now;
        p.auth_expire = 0;
        if (extinfo.size() <= (size_t)MAX_EXTINFO) p.extinfo = extinfo;
        p.extlen = (uint16_t)p.extinfo.size();
        peers_.emplace(uuid, std::move(p));
    } else {
        it->second.pub_addr = pub;
        it->second.lan_addr = lan;
        it->second.dev_type = dev_type;
        it->second.nattype = nattype;
        it->second.last_heartbeat = now;
        if (extinfo.size() <= (size_t)MAX_EXTINFO) it->second.extinfo = extinfo;
        it->second.extlen = (uint16_t)it->second.extinfo.size();
    }
    return true;
}

bool PeerManager::remove(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_.erase(uuid) > 0;
}

bool PeerManager::upsert_synced(const std::string& uuid, const sockaddr_in& pub,
                                const sockaddr_in& lan, uint8_t dev_type, uint8_t nattype,
                                const std::string& extinfo, time_t hb_time) {
    if (uuid.empty() || uuid.size() > MAX_UUID_LEN) return false;

    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(uuid);
    if (it == peers_.end()) {
        Peer p;
        p.uuid = uuid;
        p.pub_addr = pub;
        p.lan_addr = lan;
        p.dev_type = dev_type;
        p.nattype = nattype;
        p.last_heartbeat = hb_time;
        p.register_time = hb_time;
        p.auth_expire = hb_time + 3600;   // 源侧鉴权通过视为已鉴权
        if (extinfo.size() <= (size_t)MAX_EXTINFO) p.extinfo = extinfo;
        p.extlen = (uint16_t)p.extinfo.size();
        peers_.emplace(uuid, std::move(p));
    } else {
        it->second.pub_addr = pub;
        it->second.lan_addr = lan;
        it->second.dev_type = dev_type;
        it->second.nattype = nattype;
        it->second.last_heartbeat = hb_time;
        if (extinfo.size() <= (size_t)MAX_EXTINFO) it->second.extinfo = extinfo;
        it->second.extlen = (uint16_t)it->second.extinfo.size();
        it->second.auth_expire = hb_time + 3600;
    }
    return true;
}

bool PeerManager::exists(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_.find(uuid) != peers_.end();
}

bool PeerManager::get(const std::string& uuid, Peer& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(uuid);
    if (it == peers_.end()) return false;
    out = it->second;
    return true;
}

void PeerManager::set_auth_expire(const std::string& uuid, time_t expire) {
    std::lock_guard<std::mutex> lk(mu_);
    auth_sessions_[uuid] = expire;          // 登录即记，独立于在线状态
    auto it = peers_.find(uuid);
    if (it != peers_.end()) it->second.auth_expire = expire;
}

bool PeerManager::authed(const std::string& uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    time_t now = time(nullptr);
    auto as = auth_sessions_.find(uuid);
    if (as != auth_sessions_.end() && as->second > now) return true;
    auto it = peers_.find(uuid);
    if (it == peers_.end()) return false;
    return it->second.auth_expire > now;
}

void PeerManager::cleanup_auth_sessions() {
    std::lock_guard<std::mutex> lk(mu_);
    time_t now = time(nullptr);
    for (auto it = auth_sessions_.begin(); it != auth_sessions_.end();) {
        if (it->second <= now) it = auth_sessions_.erase(it);
        else ++it;
    }
}

size_t PeerManager::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_.size();
}

size_t PeerManager::device_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& kv : peers_) if (kv.second.dev_type == 1) n++;
    return n;
}

size_t PeerManager::client_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& kv : peers_) if (kv.second.dev_type == 2) n++;
    return n;
}

size_t PeerManager::authed_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    time_t now = time(nullptr);
    for (auto& kv : peers_) if (kv.second.auth_expire > now) n++;
    return n;
}

void PeerManager::cleanup_timeout(int timeout_sec) {
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (now - it->second.last_heartbeat > timeout_sec)
            it = peers_.erase(it);
        else
            ++it;
    }
}

std::vector<std::string> PeerManager::cleanup_timeout_and_collect(int timeout_sec) {
    time_t now = time(nullptr);
    std::vector<std::string> removed;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (now - it->second.last_heartbeat > timeout_sec) {
            removed.push_back(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<Peer> PeerManager::snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Peer> v;
    v.reserve(peers_.size());
    for (auto& kv : peers_) v.push_back(kv.second);
    return v;
}

void PeerManager::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    peers_.clear();
}

} // namespace p2p
