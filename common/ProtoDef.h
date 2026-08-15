#ifndef P2P_PROTO_DEF_H
#define P2P_PROTO_DEF_H

#include <cstdint>

// =============================================================================
// xnat_p2p 风格私有协议定义（自研 XN 8 字节头封装）
// 二进制分析对象：p2p_server(NatServer) + proxy_server(P2PProxy)，见 分析报告.md
// 本实现为同架构干净重实现，协议方案见 P2P服务器实现项目计划书.md 6.1(方案B)
//   - NatServer：UDP 汇聚/打洞协调（注册、心跳、地址交换、NAT 类型探测、
//                 CONNECT 协调、鉴权、设备/服务器列表、防滥用、状态查询）
//   - P2PProxy ：UDP 中继兜底（代理注册、RELAY 转发、打洞协助、可用性查询）
//   - 客户端    ：SDK 分层实现（传输/打洞/NAT检测/会话通道/可靠UDP）
// =============================================================================

namespace p2p {

constexpr uint16_t NAT_MAGIC  = 0x584E;   // "XN" 报文头魔数
constexpr uint8_t  PROTO_VER  = 0x02;     // 协议版本

constexpr int MAX_UUID_LEN = 32;          // UUID 最大长度
constexpr int MAX_IP_LEN   = 16;          // 点分十进制 IPv4 最大长度
constexpr int MAX_PKT      = 2048;        // 单报文最大缓冲
constexpr int MAX_EXTINFO  = 1024;        // 扩展信息上限（防滥用）

// -------------------------------------------------------------------------
// 统一报文头：8 字节，固定
//   +--------+---------+---------+-----------+
//   | magic  | version | msg_id  |  length   |
//   | (2B,BE)|  (1B)   |  (1B)   |  (4B,BE)  |
//   +--------+---------+---------+-----------+
// magic/length 统一网络字节序（大端）
// -------------------------------------------------------------------------
#pragma pack(push, 1)

struct MsgHead {
    uint16_t magic;      // NAT_MAGIC
    uint8_t  version;    // PROTO_VER
    uint8_t  msg_id;     // 见 MsgId
    uint32_t length;     // payload 长度（不含头）
};

// -------------------------------------------------------------------------
// 消息类型
//   0x01~0x1A  NatServer 侧
//   0x20~0x24  P2PProxy 侧
// -------------------------------------------------------------------------
enum MsgId : uint8_t {
    MSG_HEARTBEAT_REQ         = 0x01,  // 心跳注册:      UuidReq+[extinfo] -> ExtInfoRsp
    MSG_HEARTBEAT_RSP         = 0x02,  // 心跳应答(公网地址)
    MSG_SND_EXTINFO_REQ       = 0x03,  // 上报本机地址:   ExtInfoReq -> 转发目标
    MSG_ASK_EXTINFO_REQ       = 0x04,  // 查询公网IP/端口:ExtInfoReq -> ExtInfoRsp
    MSG_ASK_EXTINFO_RSP       = 0x05,
    MSG_CONNECT_REQ           = 0x06,  // 连接请求:       ConnectReq
    MSG_CONNECT_ACK           = 0x07,  // 应答发起方:     ConnectAck
    MSG_CONNECT_INVITE        = 0x08,  // 通知目标方:     ConnectInvite
    MSG_GET_DEV_LIST_REQ      = 0x09,  // 设备列表查询:   DevListReq -> DevListRsp(+entry*n)
    MSG_GET_DEV_LIST_RSP      = 0x0A,
    MSG_GET_SERVER_LIST_REQ   = 0x0B,  // 服务器列表查询: ServerListReq -> ServerListRsp
    MSG_GET_SERVER_LIST_RSP   = 0x0C,
    MSG_ADD_UID_REQ           = 0x0D,  // 授权 UID 增:    UuidReq
    MSG_DELETE_UID_REQ        = 0x0E,  // 授权 UID 删:    UuidReq
    MSG_CHECK_UID_REQ         = 0x0F,  // 校验在线/授权:  ExtInfoReq -> MSG_CHECK_UID_RSP
    MSG_SP_ASK_EXTINFO_REQ    = 0x10,  // NatServer -> Proxy 可用性查询
    MSG_SP_ASK_EXTINFO_RSP    = 0x11,  // Proxy -> NatServer 可用性应答
    MSG_CHECK_UID_RSP         = 0x12,  // 1 字节 result(0/1)
    MSG_NAT_DETECT_REQ        = 0x13,  // NAT 类型探测（双 socket 应答，快速路径）
    MSG_NAT_DETECT_RSP        = 0x14,  // NatDetectRsp（server_index 区分主/备 socket）
    MSG_AUTH_CHALLENGE_REQ    = 0x15,  // 鉴权挑战请求:   AuthChallengeReq
    MSG_AUTH_CHALLENGE_RSP    = 0x16,  // 鉴权挑战应答:   AuthChallengeRsp(nonce)
    MSG_AUTH_LOGIN_REQ        = 0x17,  // 鉴权登录:       AuthLoginReq(uuid+nonce+mac)
    MSG_AUTH_LOGIN_RSP        = 0x18,  // 鉴权结果:       AuthLoginRsp
    MSG_ADMIN_STATS_REQ       = 0x19,  // 管理统计查询（空负载）
    MSG_ADMIN_STATS_RSP       = 0x1A,  // AdminStatsRsp
    MSG_ADMIN_BLACKLIST_REQ   = 0x1D,  // 管理黑名单增删查:   AdminBlacklistReq
    MSG_ADMIN_BLACKLIST_RSP   = 0x1E,  // 管理黑名单应答:     AdminBlacklistRsp
    MSG_ICE_SDP               = 0x1F,  // #19 ICE SDP 中转（按 dst_uuid 投递）
    MSG_HEARTBEAT_REQ_ENC     = 0x1B,  // 加密心跳注册: HeartbeatReqEnc（开启鉴权后使用）
    MSG_HEARTBEAT_RSP_ENC     = 0x1C,  // 加密心跳应答: HeartbeatRspEnc
    // ---- NatServer 间注册表同步（服务器间直连，不经 Proxy）----
    MSG_SYNC_PEER_ENTRY       = 0x30,  // 单条对等节点增/更: SyncPeerEntry
    MSG_SYNC_PEER_DEL         = 0x31,  // 对等节点删除:      SyncPeerDel
    MSG_SYNC_SNAPSHOT_REQ     = 0x32,  // 全量快照请求(SyncSnapshotReq，应答为多条 0x30)
    // ---- Proxy 侧 ----
    MSG_PROXY_REGISTER_REQ    = 0x20,  // 代理注册:       ProxyRegReq -> ProxyRegRsp
    MSG_PROXY_REGISTER_RSP    = 0x21,
    MSG_PROXY_UNREGISTER_REQ  = 0x22,  // 注销注册
    MSG_PROXY_RELAY_DATA      = 0x23,  // 中继数据:       RelayFrame + TunnelFrame + 负载
    MSG_PROXY_PUNCH_HELPER    = 0x24,  // 代理协助打洞:   返回目标当前公网地址
    // ---- 低功耗唤醒（wakeserver，P7）----
    MSG_WAKE_KEEPALIVE        = 0x40,  // 设备保活报到:   WakeKeepalive -> WakeResult
    MSG_WAKE_TRIGGER          = 0x41,  // 请求唤醒设备:   WakeTrigger   -> WakeResult
    MSG_WAKE_RESULT           = 0x42,  // 保活/触发应答:  WakeResult
    MSG_WAKE_POKE             = 0x43,  // 唤醒包（服务器 -> 设备上次公网地址）
    // ---- 局域网发现（对标 TUTK LAN Search，不经 NatServer）----
    MSG_LAN_QUERY             = 0x50,  // 查询同网段 UID: LanBeacon
    MSG_LAN_ANNOUNCE          = 0x51,  // 宣告本机 UID/端口: LanBeacon
};

// 局域网组播发现（管理范围 239.255/16，TTL=1 不出网段）
constexpr const char* LAN_MCAST_IP   = "239.255.77.89";
constexpr uint16_t    LAN_MCAST_PORT = 17890;

// -------------------------------------------------------------------------
// NAT 类型（客户端自检 + 服务端记录）
// -------------------------------------------------------------------------
enum NatType : uint8_t {
    NAT_UNKNOWN        = 0,
    NAT_FULL_CONE      = 1,   // 开放型（无端口/源IP过滤）
    NAT_PORT_RESTRICTED = 2,  // 端口过滤型（受限锥型/端口受限锥型）
    NAT_SYMMETRIC      = 3,   // 对称型（需端口预测/喷洒或多代理兜底）
};

// 代理候选地址（CONNECT ack/invite 携带 3 组，对标原版 3 组代理）
struct ProxyCandidate {
    char     ip[MAX_IP_LEN];
    uint16_t port;            // 网络序
};

// -------------------------------------------------------------------------
// 业务负载结构体（数值字段均网络字节序）
// -------------------------------------------------------------------------

// 心跳 / 主动注册 / UID 授权增删
struct UuidReq {
    char     uuid[MAX_UUID_LEN + 1];
    uint8_t  dev_type;        // 0=未知 1=设备 2=App/客户端
    uint8_t  nattype;         // 客户端自检的 NAT 类型（见 NatType）
    uint16_t lan_port;        // 本机 UDP 端口（网络序）
    uint32_t session_pts;     // 端口/会话号（对标原版 PTS）
    uint16_t extlen;          // 后续扩展信息长度（网络序，0~MAX_EXTINFO）
    // 扩展信息紧跟其后（extlen 字节）
};

// 地址信息请求（仅需 uuid）
struct ExtInfoReq {
    char uuid[MAX_UUID_LEN + 1];
};

// 公网地址应答 / 心跳应答
struct ExtInfoRsp {
    uint8_t  result;          // 0=ok 1=需鉴权 2=拒绝/黑名单
    char     pub_ip[MAX_IP_LEN];
    uint16_t pub_port;        // 网络序
    char     lan_ip[MAX_IP_LEN];
    uint16_t lan_port;        // 网络序
    uint16_t nat_sock2_port;  // 网络序：备用探测端口（NAT 检测用）
};

// 连接请求（可选尾随 ConnectToken，EnableConnectToken=1 时必带）
struct ConnectReq {
    char src_uuid[MAX_UUID_LEN + 1];
    char dst_uuid[MAX_UUID_LEN + 1];
};

// 连线 Token（挂在 ConnectReq 之后）：授权 src 连接 dst
//   mac = HMAC-SHA256(AuthKey_dst, "P2P-CONN-TOKEN:" || dst || src || expire || nonce)
struct ConnectToken {
    uint32_t expire;          // unix 秒，网络序
    uint8_t  nonce[16];
    uint8_t  mac[32];
};

// 应答发起方：携带目标公网/私网地址、NAT 类型与多组兜底代理
struct ConnectAck {
    uint8_t  result;          // 0=成功 1=目标不在线 2=目标不存在
    char     dst_uuid[MAX_UUID_LEN + 1];
    char     dst_pub_ip[MAX_IP_LEN];
    uint16_t dst_pub_port;    // 网络序
    char     dst_lan_ip[MAX_IP_LEN];
    uint16_t dst_lan_port;    // 网络序
    uint8_t  dst_nattype;     // 目标 NAT 类型
    uint8_t  proxy_count;     // 可用代理候选数（0~3）
    ProxyCandidate proxies[3];
};

// 通知目标方：携带发起方公网/私网地址、NAT 类型与多组兜底代理
struct ConnectInvite {
    char     src_uuid[MAX_UUID_LEN + 1];
    char     src_pub_ip[MAX_IP_LEN];
    uint16_t src_pub_port;    // 网络序
    char     src_lan_ip[MAX_IP_LEN];
    uint16_t src_lan_port;    // 网络序
    uint8_t  src_nattype;     // 发起方 NAT 类型
    uint8_t  proxy_count;     // 可用代理候选数
    ProxyCandidate proxies[3];
};

// 设备列表查询
struct DevListReq {
    uint16_t start_index;     // 网络序
    uint16_t want_num;        // 网络序
};

// 设备列表应答（定长头 + DevListEntry * count）
struct DevListRsp {
    uint16_t total;           // 当前在线总数（网络序）
    uint16_t count;           // 本包条数（网络序）
    uint16_t start_index;     // 网络序
};

struct DevListEntry {
    char     uuid[MAX_UUID_LEN + 1];
    char     ip[MAX_IP_LEN];
    uint16_t port;            // 网络序
    uint8_t  dev_type;
};

// 服务器列表查询（可选携带本端 UID，用于 REGION 就近排序；空 uuid=按本节点区域）
struct ServerListReq {
    char uuid[MAX_UUID_LEN + 1];
};

// 服务器列表应答（定长头 + nat_count + proxy_count 个 MAX_IP_LEN 字节 IP 串）
struct ServerListRsp {
    uint16_t nat_count;       // 网络序
    uint16_t proxy_count;     // 网络序
};

// NatServer -> Proxy 可用性查询
struct ProxyAvailReq {
    char     nat_ip[MAX_IP_LEN];
    uint16_t nat_port;        // 网络序
};

// Proxy 可用性应答
struct ProxyAvailRsp {
    uint8_t  available;       // 0=忙 1=可用
    uint16_t used;            // 已用代理数（网络序）
    uint16_t max_proxy;       // 上限（网络序）
};

// 代理注册
struct ProxyRegReq {
    char uuid[MAX_UUID_LEN + 1];
    uint8_t hmac[32];   // HMAC-SHA256(proxy_auth_key, uuid)，防伪造注册
};

// 代理注册应答
struct ProxyRegRsp {
    uint8_t  result;          // 0=成功 1=代理已满 2=参数错误 3=未鉴权
    char     pub_ip[MAX_IP_LEN];
    uint16_t pub_port;        // 网络序
};

// NAT 类型探测应答（主/备 socket 各回一条，server_index 区分）
struct NatDetectRsp {
    uint8_t  server_index;    // 0=主 socket 1=备用 socket
    char     server_ip[MAX_IP_LEN];   // 该 socket 的服务地址
    uint16_t server_port;     // 网络序
    char     mapped_ip[MAX_IP_LEN];   // 客户端在该 socket 上观察到的公网映射
    uint16_t mapped_port;     // 网络序
    uint16_t main_port;       // 网络序：主 socket 端口
    uint16_t alt_port;        // 网络序：备用 socket 端口
};

// 鉴权挑战请求
struct AuthChallengeReq {
    char uuid[MAX_UUID_LEN + 1];
};

// 鉴权挑战应答
struct AuthChallengeRsp {
    uint8_t  result;          // 0=ok 1=黑名单 2=白名单拒绝 3=重复请求
    uint8_t  nonce[16];
    uint16_t nonce_ttl;       // 网络序：nonce 有效期（秒）
};

// 鉴权登录请求（mac = HMAC-SHA256(secret, uuid || nonce)）
struct AuthLoginReq {
    char     uuid[MAX_UUID_LEN + 1];
    uint8_t  nonce[16];
    uint8_t  mac[32];
};

// 鉴权登录应答
struct AuthLoginRsp {
    uint8_t  result;          // 0=成功 1=nonce失效 2=MAC错误 3=黑名单 4=白名单拒绝
    uint16_t session_ttl;     // 网络序：鉴权有效期（秒），超时需重新鉴权
};

// 管理统计应答
struct AdminStatsRsp {
    uint32_t total_pkts;      // 累计收包（网络序）
    uint32_t peers_online;    // 在线节点
    uint32_t devices;         // 设备数
    uint32_t apps;            // App 数
    uint32_t blacklist_ips;   // 封禁 IP 数
    uint32_t authed_peers;    // 已鉴权节点
    uint32_t proxy_ok;        // 可用代理数
    uint32_t connect_ok;      // CONNECT 成功次数
    uint32_t connect_fail;    // CONNECT 失败次数
};

// 管理黑名单操作（msg_id=MSG_ADMIN_BLACKLIST_REQ）
//   鉴权：mac = HMAC-SHA256(AdminSecret, op(1B) || key(64B 定长补零))
struct AdminBlacklistReq {
    uint8_t  op;              // 1=加IP 2=删IP 3=加UUID 4=删UUID 5=查询
    char     key[64];         // IP 点分十进制 / UUID（定长补零）
    uint8_t  mac[32];         // 校验标签，防伪造
};

// 管理黑名单应答（msg_id=MSG_ADMIN_BLACKLIST_RSP）
//   查询时返回首 28 条；其余仅返回数量（保证 头+负载 <= MAX_PKT）
struct AdminBlacklistEntry {
    char     key[64];         // IP / UUID
    uint32_t expire;          // 网络序：过期 epoch（0=永久）
};

struct AdminBlacklistRsp {
    uint8_t  result;          // 0=ok 1=bad_mac 2=bad_op 3=not_found 4=bad_key
    uint32_t ip_count;        // 网络序：封禁 IP 总数
    uint32_t uuid_count;      // 网络序：封禁 UUID 总数
    uint8_t  entry_count;     // 查询：本应答携带条数
    AdminBlacklistEntry entries[28];
};

// 加密心跳请求负载（MSG_HEARTBEAT_REQ_ENC，开启鉴权后使用）
//   定长头明文，其后为流加密的 UuidReq（含扩展信息）
//   密钥流见 Crypto.h p2p_stream_xor
struct HeartbeatReqEnc {
    char     uuid[MAX_UUID_LEN + 1];  // 明文：路由 / 密钥派生（33B 补零）
    uint8_t  iv[8];                   // 随机 IV
    // 密文紧随其后：cipher(UuidReq 定长部分 + 扩展信息)
};

// 加密心跳应答负载（MSG_HEARTBEAT_RSP_ENC）
struct HeartbeatRspEnc {
    char     uuid[MAX_UUID_LEN + 1];  // 明文（诊断用）
    uint8_t  iv[8];                   // 复用请求 IV
    // 密文紧随其后：cipher(ExtInfoRsp)
};

// 中继帧：源/目标 UUID 定长，其后为 TunnelFrame + 业务负载
struct RelayFrame {
    char src_uuid[MAX_UUID_LEN + 1];
    char dst_uuid[MAX_UUID_LEN + 1];
};

// -------------------------------------------------------------------------
// 对端隧道数据（打洞直连 或 中继转发的数据面帧，位于 RelayFrame 之后或裸发）
// -------------------------------------------------------------------------
constexpr uint16_t TUNNEL_MAGIC   = 0x5054;  // "PT" 隧道帧魔数
constexpr uint8_t  TUNNEL_VER     = 0x01;
constexpr int      MAX_TUNNEL_PAYLOAD = 1200;

enum TunnelType : uint8_t {
    TT_DATA  = 0,   // 业务数据（可靠通道带 seq，接收方回 TT_ACK）
    TT_ACK   = 1,   // 确认帧（ack 字段为累计确认号）
    TT_PING  = 2,   // 隧道保活
    TT_PONG  = 3,   // 保活应答
    TT_CLOSE = 4,   // 会话关闭
    TT_FEC   = 5,   // 不可靠通道前向纠错帧（seq=组基序号，ack=组内帧数，
                    //   负载 = XOR of [len(2B)|channel(1B)|payload 补零]，可恢复单帧丢失）
};

constexpr uint8_t TF_RELIABLE = 0x01;  // flags：可靠通道
constexpr uint8_t TF_ENC      = 0x02;  // flags：负载流加密（iv[8]+密文）

// 隧道负载加密的密钥流域分隔标签（33B 定长，仅作 PRF 输入，无需保密）
constexpr char TUNNEL_ENC_LABEL[33] = "P2P-TUNNEL-LABEL";

// 对端间隧道密钥派生：HMAC-SHA256(AuthSecret, "P2P-TUNNEL-KEY:" || min(uuid) || ":" || max(uuid))
constexpr char TUNNEL_KEY_PREFIX[] = "P2P-TUNNEL-KEY:";

// -------------------------------------------------------------------------
// NatServer 间注册表同步（SyncPeers=1 时启用）
//   增量：本地节点增/更/删立即广播 MSG_SYNC_PEER_ENTRY / MSG_SYNC_PEER_DEL；
//   全量：启动/周期发 MSG_SYNC_SNAPSHOT_REQ，对端以多条 MSG_SYNC_PEER_ENTRY 应答
//   防环：hop 逐跳 +1，超过 SYNC_HOP_MAX 停止转发；快照应答 hop=SYNC_HOP_SNAP 不转发
// -------------------------------------------------------------------------
constexpr uint8_t  SYNC_HOP_MAX    = 3;     // 增量广播最大转发跳数
constexpr uint8_t  SYNC_HOP_SNAP   = 0xFF;  // 快照条目：仅落地，不再转发
constexpr uint32_t SYNC_SNAPSHOT_INTERVAL = 30;  // 全量快照周期（秒）

// 对等节点同步条目（服务器间转发一条注册表记录）
struct SyncPeerEntry {
    char     uuid[MAX_UUID_LEN + 1];
    char     pub_ip[MAX_IP_LEN];
    uint16_t pub_port;          // 网络序：对端观察到的公网端口
    char     lan_ip[MAX_IP_LEN];
    uint16_t lan_port;          // 网络序：上报的私网端口
    uint8_t  dev_type;          // 0=未知 1=设备 2=App/客户端
    uint8_t  nattype;           // 客户端自检 NAT 类型
    uint8_t  hop;               // 转发跳数（SYNC_HOP_MAX 停止；SYNC_HOP_SNAP 不转发）
    uint8_t  reserved;
    uint32_t hb_time;           // 网络序：源服务器侧最后心跳时间（epoch）
    uint16_t extlen;            // 网络序：扩展信息长度
    // 扩展信息紧跟其后（extlen 字节，<= MAX_EXTINFO）
};

// 对等节点删除
struct SyncPeerDel {
    char uuid[MAX_UUID_LEN + 1];
};

// 全量快照请求（带时间戳防重放；末尾由广播逻辑附加 HMAC，接收端校验 300s 新鲜度）
struct SyncSnapshotReq {
    uint32_t ts;              // 网络序：请求时刻 epoch
};

struct TunnelFrame {
    uint16_t magic;        // TUNNEL_MAGIC（网络序）
    uint8_t  version;      // TUNNEL_VER
    uint8_t  type;         // TunnelType
    uint16_t session_id;   // 会话号（网络序）
    uint8_t  channel_id;   // 逻辑通道号
    uint8_t  flags;        // TF_*
    uint16_t seq;          // 通道内发送序号（网络序）
    uint16_t ack;          // 累计确认号（网络序）
    uint16_t len;          // 负载长度（网络序，<= MAX_TUNNEL_PAYLOAD）
};

// 低功耗唤醒保活（设备 -> wakeserver）
struct WakeKeepalive {
    char    uuid[MAX_UUID_LEN + 1];
    uint8_t hmac[32];         // 可选：HMAC-SHA256(WakeSecret, uuid)；无密钥则全 0
};

// 请求唤醒指定 UID（客户端 / NatServer -> wakeserver）
struct WakeTrigger {
    char uuid[MAX_UUID_LEN + 1];
};

// 保活/触发应答；POKE 复用 uuid 字段通知设备
struct WakeResult {
    uint8_t result;           // 见 WakeStatus
    char    uuid[MAX_UUID_LEN + 1];
};

#pragma pack(pop)

// 结果码
enum ConnectResult : uint8_t {
    CONNECT_OK         = 0,
    CONNECT_OFFLINE    = 1,
    CONNECT_NOT_FOUND  = 2,
    CONNECT_NEED_AUTH  = 3,   // 发起方尚未完成 AUTH_LOGIN
    CONNECT_BAD_TOKEN  = 4,   // 连线 Token 缺失/过期/MAC 错误
};

enum AuthResult : uint8_t {
    AUTH_OK            = 0,
    AUTH_BAD_NONCE     = 1,
    AUTH_BAD_MAC       = 2,
    AUTH_BLACKLIST     = 3,
    AUTH_WHITELIST_REJ = 4,
};

enum WakeStatus : uint8_t {
    WAKE_OK        = 0,   // 保活已记 / 已向设备发 POKE
    WAKE_NOT_FOUND = 1,   // 无保活记录（设备未报到或已过期）
    WAKE_BAD_MAC   = 2,   // 保活 HMAC 校验失败
};


// #19 ICE SDP 中转消息（变长）
// dst 供 NatServer 路由；src 供接收端落到对应 Conn（避免 SDP 早于 INVITE 时无法认领）
struct IceSdpMsg {
    char     dst_uuid[MAX_UUID_LEN + 1];
    char     src_uuid[MAX_UUID_LEN + 1];
    uint16_t sdp_len;                // 网络序：sdp 字节数
    char     sdp[1];                 // 变长，实际长度由 sdp_len 指定
};

// 局域网发现信标（QUERY / ANNOUNCE 共用）
struct LanBeacon {
    char     uuid[MAX_UUID_LEN + 1];     // QUERY: 要找的 UID（空=任意）；ANNOUNCE: 本机 UID
    char     src_uuid[MAX_UUID_LEN + 1]; // 发送方 UID（被查方据此缓存查询者）
    uint16_t lan_port;                   // 网络序：主信令/媒体 socket 端口
};
} // namespace p2p

#endif // P2P_PROTO_DEF_H
