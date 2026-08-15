#include "CfgFile.h"
#include "File.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

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

static bool to_bool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes";
}

static std::vector<std::string> split_csv(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static char parse_region_char(const std::string& v) {
    if (v.empty()) return 0;
    char r = v[0];
    if (r >= 'a' && r <= 'z') r = (char)(r - 'a' + 'A');
    const bool ok = (r >= 'A' && r <= 'Z') || (r >= '2' && r <= '7');
    return ok ? r : 0;
}

// "ip:R,ip:R" → map[ip]=REGION
static void parse_ip_regions(const std::string& v,
                             std::unordered_map<std::string, char>& out) {
    for (auto item : split_csv(v)) {
        trim(item);
        size_t pos = item.rfind(':');
        if (pos == std::string::npos || pos + 1 >= item.size()) continue;
        std::string ip = item.substr(0, pos);
        trim(ip);
        char r = parse_region_char(item.substr(pos + 1));
        if (valid_ip(ip) && r) out[ip] = r;
    }
}

// 范围校验的整数解析：越界/非法保持原值
template <typename T>
static void set_int_in_range(T& field, const std::string& val, long min_v, long max_v) {
    const long n = atol(val.c_str());
    if (n >= min_v && n <= max_v) field = (T)n;
}

// 表驱动的配置项注册：新增配置只需加一行（前缀键 NatServer*/Proxy* 单独处理）
static void parse_value(CfgData& out, const std::string& key, const std::string& val) {
    if (key.rfind("NatServer", 0) == 0) {
        std::string ip;
        if (split_ip_port(val, ip, nullptr)) out.nat_ips.push_back(ip);
        else LOGE("CfgFile", "bad NatServer value: %s", val.c_str());
        return;
    }
    // Proxy1_1 / Proxy2_3 …（必须以数字开头，避免吃掉 ProxyRegions）
    if (key.rfind("Proxy", 0) == 0 && key.size() > 5 &&
        key[5] >= '0' && key[5] <= '9') {
        std::string ip;
        if (split_ip_port(val, ip, nullptr)) out.proxy_ips.push_back(ip);
        else LOGE("CfgFile", "bad Proxy value: %s", val.c_str());
        return;
    }

    using Setter = void (*)(CfgData&, const std::string&);
    static const std::unordered_map<std::string, Setter> kSetters = {
        {"AuthSecret",     [](CfgData& c, const std::string& v) { c.auth_secret = v; }},
        {"EnableAuth",     [](CfgData& c, const std::string& v) { c.enable_auth = to_bool(v); }},
        {"UidStrict",      [](CfgData& c, const std::string& v) { c.uid_strict = to_bool(v); }},
        {"EnableLicense",  [](CfgData& c, const std::string& v) { c.enable_license = to_bool(v); }},
        {"LicenseFile",    [](CfgData& c, const std::string& v) { c.license_file = v; }},
        {"LicensePass",    [](CfgData& c, const std::string& v) { c.license_pass = v; }},
        {"AllowedUuids",   [](CfgData& c, const std::string& v) { c.allowed_uuids = split_csv(v); }},
        {"WhitelistUuids", [](CfgData& c, const std::string& v) {
             c.allowed_uuids = split_csv(v);
             c.enable_license = true;   // 别名：写即启用白名单
         }},
        {"ProcWorkers",       [](CfgData& c, const std::string& v) { set_int_in_range(c.proc_workers, v, 1, 64); }},
        {"RecvThreads",       [](CfgData& c, const std::string& v) { set_int_in_range(c.recv_threads, v, 1, 16); }},
        {"FloodPktThreshold", [](CfgData& c, const std::string& v) { set_int_in_range(c.flood_pkt_threshold, v, 1, 0x7FFFFFFF); }},
        {"BlacklistSeconds",  [](CfgData& c, const std::string& v) { set_int_in_range(c.blacklist_seconds, v, 1, 0x7FFFFFFF); }},
        {"NatSock2Port",      [](CfgData& c, const std::string& v) { set_int_in_range(c.nat_sock2_port, v, 1, 65535); }},
        {"SyncPeers",      [](CfgData& c, const std::string& v) { c.sync_enabled = to_bool(v); }},
        {"SyncAddrs",      [](CfgData& c, const std::string& v) { c.sync_addrs = split_csv(v); }},
        {"SyncAuthSecret", [](CfgData& c, const std::string& v) { c.sync_auth_secret = v; }},
        {"AdminSecret",    [](CfgData& c, const std::string& v) { c.admin_secret = v; }},
        {"BlacklistFile",  [](CfgData& c, const std::string& v) { c.blacklist_file = v; }},
        {"BlacklistPass",  [](CfgData& c, const std::string& v) { c.blacklist_pass = v; }},
        {"Region",         [](CfgData& c, const std::string& v) { c.region = parse_region_char(v); }},
        {"NatRegions",     [](CfgData& c, const std::string& v) { parse_ip_regions(v, c.nat_regions); }},
        {"ProxyRegions",   [](CfgData& c, const std::string& v) { parse_ip_regions(v, c.proxy_regions); }},
        {"WakeServer",     [](CfgData& c, const std::string& v) { c.wake_server = v; }},
    };
    auto it = kSetters.find(key);
    if (it != kSetters.end()) it->second(out, val);
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
    LOGI("CfgFile", "loaded %s: nat[%zu] proxy[%zu] auth[%d] license[%d] region[%c]",
         path.c_str(), out.nat_ips.size(), out.proxy_ips.size(),
         (int)out.enable_auth, (int)out.enable_license,
         out.region ? out.region : '-');
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
