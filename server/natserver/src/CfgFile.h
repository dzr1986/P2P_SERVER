#ifndef P2P_CFG_FILE_H
#define P2P_CFG_FILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// P2pServers.cfg 配置解析（支持热加载）
// 格式（key=value，# 注释）：
//   NatServer1=1.2.3.4[:port]      NAT 汇聚服务端（可多个）
//   Proxy1_1=1.2.3.4[:port]        中继代理（可多个）
//   AuthSecret=xxx                  鉴权密钥（空=关闭鉴权）
//   EnableAuth=0|1                  是否强制鉴权
//   EnableLicense=0|1               是否启用 UUID 白名单
//   AllowedUuids=u1,u2,...          白名单 UUID（EnableLicense=1 时生效）
//   LicenseFile=<path>              白名单持久化文件（.dat/.lic）
//   LicensePass=<pass>              白名单文件加密口令（空=明文存取）
//   ProcWorkers=N                   业务处理线程数
//   RecvThreads=N                   UDP 收包线程数（SO_REUSEPORT，>=1）
//   FloodPktThreshold=N             防洪水阈值（pps）
//   BlacklistSeconds=N              IP 拉黑时长（秒）
//   NatSock2Port=N                  备用探测端口（0=主端口+1）
//   SyncPeers=0|1                   是否启用多 NatServer 注册表同步
//   SyncAddrs=ip[:port],...         同步对端（缺省用 NatServer* 列表；port 缺省取本机 nat 端口）
//   SyncAuthSecret=xxx              同步消息签名密钥（空=不签名，不推荐生产）
//   AdminSecret=xxx                 管理接口（黑名单增删）授权密钥（空=关闭管理接口）
//   BlacklistFile=<path>            IP/UUID 黑名单持久化文件
//   BlacklistPass=<pass>            黑名单文件加密口令（空=明文存取）
//   UidStrict=0|1                   仅接受结构化 UID（20 字符 Base32+CRC，见 common/Uid.h）
// ---------------------------------------------------------------------------
struct CfgData {
    std::vector<std::string> nat_ips;
    std::vector<std::string> proxy_ips;

    std::string auth_secret;
    bool        enable_auth = false;
    bool        uid_strict = false;     // 仅接受结构化 UID（P1：UID 体系）
    bool        enable_license = false;
    std::vector<std::string> allowed_uuids;
    std::string license_file;
    std::string license_pass;

    bool        sync_enabled = false;
    std::vector<std::string> sync_addrs;   // "ip" 或 "ip:port" 形式
    std::string sync_auth_secret;
    std::string admin_secret;

    std::string blacklist_file;
    std::string blacklist_pass;

    int         proc_workers = 4;
    int         recv_threads = 2;       // UDP 收包线程数（SO_REUSEPORT）
    uint32_t    flood_pkt_threshold = 200;
    uint32_t    blacklist_seconds = 60;
    uint16_t    nat_sock2_port = 0;    // 0 -> 主端口+1

    std::string cfg_path;
    bool        loaded = false;

    bool uuid_allowed(const std::string& uuid) const {
        if (!enable_license || allowed_uuids.empty()) return true;
        for (const auto& u : allowed_uuids) if (u == uuid) return true;
        return false;
    }
    bool auth_enabled() const { return enable_auth && !auth_secret.empty(); }
};

// 首次加载：失败返回 false（调用方决定是否继续），成功填充 out
bool load_server_set(CfgData& out, const std::string& path);

// 热加载：文件 mtime 变化则重新解析，成功返回 true
// last_mtime 由调用方持有（跨线程跟踪），避免并发读脏数据
bool reload_if_changed(CfgData& out, const std::string& path, uint64_t* last_mtime);

// 返回配置文件 mtime（供调用方初始化基线）
uint64_t cfg_file_mtime(const std::string& path);

// 常用辅助
const char* first_nat_ip(const CfgData& cfg);
const char* first_proxy_ip(const CfgData& cfg);

} // namespace p2p

#endif // P2P_CFG_FILE_H
