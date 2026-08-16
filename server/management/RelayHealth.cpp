#include "server/management/RelayHealth.h"
#include "core/foundation/RegionSched.h"
#include "core/foundation/Util.h"

#include <arpa/inet.h>
#include <cstring>

namespace p2p {

void RelayHealth::collect(const sockaddr_in& from, uint16_t proxy_port,
                          const ProxyAvailRsp& rsp) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : rows_) {
        if (p.ip == ip && p.port == proxy_port) {
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
    h.port = proxy_port;
    h.available = rsp.available;
    h.used = ntohs(rsp.used);
    h.max_proxy = ntohs(rsp.max_proxy);
    h.last_seen = time(nullptr);
    h.down = 0;
    rows_.push_back(h);
}

void RelayHealth::mark_stale() {
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : rows_) {
        if (now - p.last_seen >= 10) {
            if (p.down < 255) p.down++;
            p.available = 0;
        }
    }
}

void RelayHealth::pick(ProxyCandidate out[3], uint8_t& count,
                       const CfgData& cfg, uint16_t proxy_port, char prefer_region) {
    count = 0;
    const char local = cfg.region;
    struct Cand { std::string ip; double weight; };
    std::vector<Cand> cands;
    {
        std::lock_guard<std::mutex> lk(mu_);
        time_t now = time(nullptr);
        for (auto& p : rows_) {
            bool fresh = now - p.last_seen <= 30;
            bool healthy = fresh && p.available && p.down < 2;
            if (!healthy) continue;
            double util = p.max_proxy ? (double)p.used / p.max_proxy : 1.0;
            double w = 1.0 - util;
            if (w < 0.05) w = 0.05;
            w *= 1.0 - 0.3 * p.down;
            if (w < 0.02) w = 0.02;
            const char r = region_of(p.ip, cfg.proxy_regions, local);
            w *= region_weight_boost(r, prefer_region, local);
            cands.push_back({p.ip, w});
        }
    }

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
            out[count].port = htons(proxy_port);
            count++;
        }
    }

    std::vector<std::string> fallback = cfg.proxy_ips;
    sort_ips_by_region(fallback, cfg.proxy_regions, prefer_region, local);
    for (auto& ip : fallback) {
        if (count >= 3) break;
        if (dup(ip)) continue;
        memset(out + count, 0, sizeof(ProxyCandidate));
        strncpy(out[count].ip, ip.c_str(), MAX_IP_LEN - 1);
        out[count].port = htons(proxy_port);
        count++;
    }
    if (cfg.proxy_alt_port && count > 0 && count < 3) {
        memset(out + count, 0, sizeof(ProxyCandidate));
        memcpy(out[count].ip, out[0].ip, MAX_IP_LEN);
        out[count].ip[MAX_IP_LEN - 1] = 0;
        out[count].port = htons(cfg.proxy_alt_port);
        count++;
    }
}

uint32_t RelayHealth::available_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t ok = 0;
    for (auto& p : rows_) if (p.available) ok++;
    return ok;
}

void RelayHealth::visit(const std::function<void(const ProxyHealth&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : rows_) fn(p);
}

} // namespace p2p
