#include "CfgFile.h"
#include "File.h"
#include "Log.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace p2p {

static void trim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                          s.front() == '\n')) s.erase(s.begin());
}

static bool valid_ip(const std::string& ip) {
    if (ip.empty() || ip.size() > 15) return false;
    int seg = 0, num = 0, dots = 0;
    for (char c : ip) {
        if (c == '.') { dots++; seg = 0; num = 0; }
        else if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); if (num > 255) return false; seg++; if (seg > 3) return false; }
        else return false;
    }
    return dots == 3 && seg > 0;
}

// 解析 "ip[:port]" 形式，仅取 ip
static bool split_ip_port(const std::string& in, std::string& ip, uint16_t* port) {
    ip = in;
    trim(ip);
    if (port) *port = 0;
    size_t pos = ip.find(':');
    if (pos != std::string::npos) {
        if (port) *port = (uint16_t)atoi(ip.c_str() + pos + 1);
        ip = ip.substr(0, pos);
        trim(ip);
    }
    return valid_ip(ip);
}

static void parse_value(CfgData& out, const std::string& key, const std::string& val) {
    std::string ip;
    uint16_t p = 0;

    if (key.rfind("NatServer", 0) == 0) {
        if (split_ip_port(val, ip, &p)) out.nat_ips.push_back(ip);
        else LOGE("CfgFile", "bad NatServer value: %s", val.c_str());
    } else if (key.rfind("Proxy", 0) == 0) {
        if (split_ip_port(val, ip, &p)) out.proxy_ips.push_back(ip);
        else LOGE("CfgFile", "bad Proxy value: %s", val.c_str());
    } else if (key == "AuthSecret") {
        out.auth_secret = val;
    } else if (key == "EnableAuth") {
        out.enable_auth = (val == "1" || val == "true" || val == "yes");
    } else if (key == "EnableLicense") {
        out.enable_license = (val == "1" || val == "true" || val == "yes");
    } else if (key == "LicenseFile") {
        out.license_file = val;
    } else if (key == "LicensePass") {
        out.license_pass = val;
    } else if (key == "AllowedUuids" || key == "WhitelistUuids") {
        out.allowed_uuids.clear();
        std::string cur;
        for (char c : val) {
            if (c == ',') { if (!cur.empty()) out.allowed_uuids.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.allowed_uuids.push_back(cur);
        if (key == "WhitelistUuids") out.enable_license = true;  // 别名：写即启用白名单
    } else if (key == "ProcWorkers") {
        int n = atoi(val.c_str());
        if (n >= 1 && n <= 64) out.proc_workers = n;
    } else if (key == "RecvThreads") {
        int n = atoi(val.c_str());
        if (n >= 1 && n <= 16) out.recv_threads = n;
    } else if (key == "FloodPktThreshold") {
        uint32_t n = (uint32_t)atoi(val.c_str());
        if (n >= 1) out.flood_pkt_threshold = n;
    } else if (key == "BlacklistSeconds") {
        uint32_t n = (uint32_t)atoi(val.c_str());
        if (n >= 1) out.blacklist_seconds = n;
    } else if (key == "NatSock2Port") {
        uint16_t n = (uint16_t)atoi(val.c_str());
        if (n > 0) out.nat_sock2_port = n;
    } else if (key == "SyncPeers") {
        out.sync_enabled = (val == "1" || val == "true" || val == "yes");
    } else if (key == "SyncAddrs") {
        out.sync_addrs.clear();
        std::string cur;
        for (char c : val) {
            if (c == ',') { if (!cur.empty()) out.sync_addrs.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.sync_addrs.push_back(cur);
    } else if (key == "SyncAuthSecret") {
        out.sync_auth_secret = val;
    } else if (key == "AdminSecret") {
        out.admin_secret = val;
    } else if (key == "BlacklistFile") {
        out.blacklist_file = val;
    } else if (key == "BlacklistPass") {
        out.blacklist_pass = val;
    }
    // 未知 key 忽略
}

static bool parse_file(CfgData& out, const std::string& path) {
    CfgData fresh;
    fresh.cfg_path = path;
    fresh.loaded = false;

    FileHandle fp = open_file(path, "r");
    if (!fp) {
        LOGW("CfgFile", "open %s failed", path.c_str());
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp.get())) {
        std::string s = line;
        trim(s);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        trim(key);
        trim(val);
        if (key.empty()) continue;
        parse_value(fresh, key, val);
    }

    if (fresh.nat_ips.empty()) fresh.nat_ips.push_back("127.0.0.1");
    if (fresh.proxy_ips.empty()) fresh.proxy_ips.push_back("127.0.0.1");
    fresh.loaded = true;
    out = std::move(fresh);
    return true;
}

static uint64_t file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_mtime;
}

uint64_t cfg_file_mtime(const std::string& path) { return file_mtime(path); }

bool load_server_set(CfgData& out, const std::string& path) {
    bool ok = parse_file(out, path);
    if (!ok) {
        out.nat_ips = {"127.0.0.1"};
        out.proxy_ips = {"127.0.0.1"};
        out.cfg_path = path;
    }
    LOGI("CfgFile", "loaded %s: nat[%zu] proxy[%zu] auth[%d] license[%d]",
         path.c_str(), out.nat_ips.size(), out.proxy_ips.size(),
         (int)out.enable_auth, (int)out.enable_license);
    return ok;
}

bool reload_if_changed(CfgData& out, const std::string& path, uint64_t* last_mtime) {
    uint64_t mt = file_mtime(path);
    if (mt == 0 || mt == *last_mtime) return false;
    *last_mtime = mt;
    if (parse_file(out, path)) {
        LOGI("CfgFile", "hot reload %s", path.c_str());
        return true;
    }
    return false;
}

const char* first_nat_ip(const CfgData& cfg) {
    return cfg.nat_ips.empty() ? "127.0.0.1" : cfg.nat_ips[0].c_str();
}

const char* first_proxy_ip(const CfgData& cfg) {
    return cfg.proxy_ips.empty() ? "127.0.0.1" : cfg.proxy_ips[0].c_str();
}

} // namespace p2p
