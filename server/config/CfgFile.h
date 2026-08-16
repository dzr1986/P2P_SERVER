#ifndef P2P_CFG_FILE_H
#define P2P_CFG_FILE_H

#include <cstdint>
#include <string>
#include <unordered_map>
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
//   NatSock2Port=N                  备用探测端口（0=临时端口）
//   NatSock3Port=N                  第三探测口（NAT4E；0=临时端口）
//   SyncPeers=0|1                   是否启用多 NatServer 注册表同步
//   SyncAddrs=ip[:port],...         同步对端（缺省用 NatServer* 列表；port 缺省取本机 nat 端口）
//   SyncAuthSecret=xxx              同步消息签名密钥（空=不签名，不推荐生产）
//   AdminSecret=xxx                 管理接口（黑名单增删）授权密钥（空=关闭管理接口）
//   BlacklistFile=<path>            IP/UUID 黑名单持久化文件
//   BlacklistPass=<pass>            黑名单文件加密口令（空=明文存取）
//   UidStrict=0|1                   仅接受结构化 UID（20 字符 Base32+CRC，见 common/Uid.h）
//   Region=C                        本节点区域（1 字符 Base32，P6 就近调度）
//   NatRegions=ip:R,ip:R            NAT 入口区域标注（缺省继承 Region）
//   ProxyRegions=ip:R,ip:R          Proxy 入口区域标注（缺省继承 Region）
//   WakeServer=ip:port              低功耗唤醒服务（CONNECT 目标离线时触发 POKE）
//   EnableConnectToken=0|1          CONNECT 必须携带连线 Token（需 AuthSecret）
//   ProxyAltPort=N                  中继 UDP 兼听端口（生产 443；0=不向客户端宣告）
//   ProxyTcpPort=N                  DERP TCP/TLS 面（生产 443；0=不宣告）
//   InstanceName=nat-cn-1           实例名（状态 JSON / 日志）
//   StatusPort=N                    HTTP 状态口（0=关；也可 P2P_STATUS_PORT）
//   StatusAllow=127.0.0.1,10.0.0.0/8  状态口来源白名单（空=不限制）
// 覆盖顺序（学 EasyTier）：文件 < P2P_* 环境变量 < 命令行。
// 文件值支持 ${ENV}；P2P_DISABLE_ENV_PARSING=1 只关展开，不关 P2P_* 覆盖。
// ---------------------------------------------------------------------------
struct CfgData {
    std::vector<std::string> nat_ips;
    std::vector<std::string> proxy_ips;
    char        region = 0;           // 本节点 REGION（0=未配置）
    std::unordered_map<std::string, char> nat_regions;
    std::unordered_map<std::string, char> proxy_regions;

    std::string auth_secret;
    bool        enable_auth = false;
    bool        uid_strict = false;     // 仅接受结构化 UID（P1：UID 体系）
    bool        enable_connect_token = false;  // CONNECT 必须出示连线 Token
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
    std::string wake_server;          // "ip:port"，空=不通知 wakeserver

    int         proc_workers = 4;
    int         recv_threads = 2;       // UDP 收包线程数（SO_REUSEPORT）
    uint32_t    flood_pkt_threshold = 200;
    uint32_t    blacklist_seconds = 60;
    uint16_t    nat_sock2_port = 0;    // 0 -> 临时端口
    uint16_t    nat_sock3_port = 0;    // 0 -> 临时端口（NAT4E）
    uint16_t    proxy_alt_port = 0;    // TURN-over-443 UDP 兼听端口（0=不宣告）
    uint16_t    proxy_tcp_port = 0;    // DERP TCP/TLS 面（0=不宣告）

    std::string instance_name;         // 空=未命名
    uint16_t    status_port = 0;       // 0=不启 StatusServer
    std::vector<std::string> status_allow;  // 空=不限制

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

// 文件加载后套一层 P2P_*（已设置的才覆盖）。单测可直接调。
void apply_cfg_env(CfgData& cfg);

// 展开 ${NAME}；未定义保持原样。disable_env_parsing 时原样返回。
std::string expand_cfg_env(const std::string& in, bool disable_env_parsing);

// StatusAllow：空规则=放行；支持 a.b.c.d 或 a.b.c.d/n。
bool ipv4_in_allow_list(uint32_t addr_be, const std::vector<std::string>& rules);

} // namespace p2p

#endif // P2P_CFG_FILE_H
