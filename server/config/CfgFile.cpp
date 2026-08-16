#include "server/config/CfgFile.h"
#include "core/foundation/File.h"
#include "core/foundation/Log.h"

#include <arpa/inet.h>
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
        {"EnableConnectToken", [](CfgData& c, const std::string& v) { c.enable_connect_token = to_bool(v); }},
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
        {"NatSock3Port",      [](CfgData& c, const std::string& v) { set_int_in_range(c.nat_sock3_port, v, 1, 65535); }},
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
        {"ProxyAltPort",   [](CfgData& c, const std::string& v) { set_int_in_range(c.proxy_alt_port, v, 1, 65535); }},
        {"ProxyTcpPort",   [](CfgData& c, const std::string& v) { set_int_in_range(c.proxy_tcp_port, v, 1, 65535); }},
        {"InstanceName",   [](CfgData& c, const std::string& v) { c.instance_name = v; }},
        {"StatusPort",     [](CfgData& c, const std::string& v) { set_int_in_range(c.status_port, v, 0, 65535); }},
        {"StatusAllow",    [](CfgData& c, const std::string& v) {
             c.status_allow = split_csv(v);
             for (auto& s : c.status_allow) trim(s);
         }},
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
        const char* no_exp = getenv("P2P_DISABLE_ENV_PARSING");
        val = expand_cfg_env(val, no_exp && no_exp[0] == '1');
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
    apply_cfg_env(out);
    LOGI("CfgFile", "loaded %s: nat[%zu] proxy[%zu] auth[%d] license[%d] region[%c] instance[%s]",
         path.c_str(), out.nat_ips.size(), out.proxy_ips.size(),
         (int)out.enable_auth, (int)out.enable_license,
         out.region ? out.region : '-',
         out.instance_name.empty() ? "-" : out.instance_name.c_str());
    return ok;
}

bool reload_if_changed(CfgData& out, const std::string& path, uint64_t* last_mtime) {
    uint64_t mt = file_mtime(path);
    if (mt == 0 || mt == *last_mtime) return false;
    *last_mtime = mt;
    if (parse_file(out, path)) {
        apply_cfg_env(out);
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

std::string expand_cfg_env(const std::string& in, bool disable_env_parsing) {
    if (disable_env_parsing) return in;
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
            const size_t end = in.find('}', i + 2);
            if (end == std::string::npos) {
                out.append(in, i, std::string::npos);
                break;
            }
            const std::string name = in.substr(i + 2, end - (i + 2));
            if (const char* ev = getenv(name.c_str())) out += ev;
            else out.append(in, i, end - i + 1);
            i = end + 1;
        } else {
            out += in[i++];
        }
    }
    return out;
}

void apply_cfg_env(CfgData& cfg) {
    auto env = [](const char* name) -> const char* { return getenv(name); };
    if (const char* v = env("P2P_AUTH_SECRET")) cfg.auth_secret = v;
    if (const char* v = env("P2P_ENABLE_AUTH")) cfg.enable_auth = to_bool(v);
    if (const char* v = env("P2P_UID_STRICT")) cfg.uid_strict = to_bool(v);
    if (const char* v = env("P2P_ENABLE_CONNECT_TOKEN")) cfg.enable_connect_token = to_bool(v);
    if (const char* v = env("P2P_ENABLE_LICENSE")) cfg.enable_license = to_bool(v);
    if (const char* v = env("P2P_LICENSE_FILE")) cfg.license_file = v;
    if (const char* v = env("P2P_LICENSE_PASS")) cfg.license_pass = v;
    if (const char* v = env("P2P_ALLOWED_UUIDS")) cfg.allowed_uuids = split_csv(v);
    if (const char* v = env("P2P_PROC_WORKERS")) set_int_in_range(cfg.proc_workers, v, 1, 64);
    if (const char* v = env("P2P_RECV_THREADS")) set_int_in_range(cfg.recv_threads, v, 1, 16);
    if (const char* v = env("P2P_FLOOD_PKT_THRESHOLD"))
        set_int_in_range(cfg.flood_pkt_threshold, v, 1, 0x7FFFFFFF);
    if (const char* v = env("P2P_BLACKLIST_SECONDS"))
        set_int_in_range(cfg.blacklist_seconds, v, 1, 0x7FFFFFFF);
    if (const char* v = env("P2P_NAT_SOCK2_PORT")) set_int_in_range(cfg.nat_sock2_port, v, 1, 65535);
    if (const char* v = env("P2P_NAT_SOCK3_PORT")) set_int_in_range(cfg.nat_sock3_port, v, 1, 65535);
    if (const char* v = env("P2P_SYNC_PEERS")) cfg.sync_enabled = to_bool(v);
    if (const char* v = env("P2P_SYNC_ADDRS")) cfg.sync_addrs = split_csv(v);
    if (const char* v = env("P2P_SYNC_AUTH_SECRET")) cfg.sync_auth_secret = v;
    if (const char* v = env("P2P_ADMIN_SECRET")) cfg.admin_secret = v;
    if (const char* v = env("P2P_BLACKLIST_FILE")) cfg.blacklist_file = v;
    if (const char* v = env("P2P_BLACKLIST_PASS")) cfg.blacklist_pass = v;
    if (const char* v = env("P2P_REGION")) cfg.region = parse_region_char(v);
    if (const char* v = env("P2P_WAKE_SERVER")) cfg.wake_server = v;
    if (const char* v = env("P2P_PROXY_ALT_PORT")) set_int_in_range(cfg.proxy_alt_port, v, 1, 65535);
    if (const char* v = env("P2P_PROXY_TCP_PORT")) set_int_in_range(cfg.proxy_tcp_port, v, 1, 65535);
    if (const char* v = env("P2P_INSTANCE_NAME")) cfg.instance_name = v;
    if (const char* v = env("P2P_STATUS_PORT")) set_int_in_range(cfg.status_port, v, 0, 65535);
    if (const char* v = env("P2P_STATUS_ALLOW")) {
        cfg.status_allow = split_csv(v);
        for (auto& s : cfg.status_allow) trim(s);
    }
}

static bool parse_ipv4_host(const std::string& s, uint32_t& host) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    host = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

bool ipv4_in_allow_list(uint32_t addr_be, const std::vector<std::string>& rules) {
    if (rules.empty()) return true;
    const uint32_t addr = ntohl(addr_be);
    for (std::string r : rules) {
        trim(r);
        if (r.empty()) continue;
        int prefix = 32;
        const size_t slash = r.find('/');
        std::string ip = r;
        if (slash != std::string::npos) {
            ip = r.substr(0, slash);
            prefix = atoi(r.c_str() + slash + 1);
            if (prefix < 0 || prefix > 32) continue;
        }
        uint32_t net = 0;
        if (!parse_ipv4_host(ip, net)) continue;
        const uint32_t mask = (prefix == 0) ? 0u : (~0u << (32 - prefix));
        if ((addr & mask) == (net & mask)) return true;
    }
    return false;
}

} // namespace p2p
