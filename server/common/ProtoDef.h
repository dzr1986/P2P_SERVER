#ifndef P2P_PROTO_DEF_H
#define P2P_PROTO_DEF_H

#include <cstdint>

// =============================================================================
// xnat_p2p 风格私有协议定义
// 二进制分析对象：p2p_server(NatServer) + proxy_server(P2PProxy)
// 本项目为同架构重新实现，协议为自研设计，思路对齐原实现：
//   - NatServer：UDP 汇聚/打洞协调（注册、心跳、地址交换、CONNECT 转发）
//   - P2PProxy ：UDP 中继兜底（代理注册、RELAY 转发、可用性查询）
// =============================================================================

namespace p2p {

constexpr uint16_t NAT_MAGIC   = 0x584E;   // "XN"  报文头魔数
constexpr uint8_t  PROTO_VER   = 0x01;     // 协议版本

constexpr int MAX_UUID_LEN = 32;           // UUID 最大长度
constexpr int MAX_IP_LEN   = 16;           // 点分十进制 IPv4 最大长度

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
//   0x01~0x12  NatServer 侧
//   0x20~0x24  P2PProxy 侧
// -------------------------------------------------------------------------
enum MsgId : uint8_t {
    MSG_HEARTBEAT_REQ         = 0x01,  // 心跳注册:      UuidReq -> ExtInfoRsp
    MSG_HEARTBEAT_RSP         = 0x02,  // 心跳应答(公网地址)
    MSG_SND_EXTINFO_REQ       = 0x03,  // 上报本机地址:   ExtInfoReq
    MSG_ASK_EXTINFO_REQ       = 0x04,  // 查询公网IP/端口:ExtInfoReq -> ExtInfoRsp
    MSG_ASK_EXTINFO_RSP       = 0x05,
    MSG_CONNECT_REQ           = 0x06,  // 连接请求:       ConnectReq
    MSG_CONNECT_ACK           = 0x07,  // 应答发起方:     ConnectAck
    MSG_CONNECT_INVITE        = 0x08,  // 通知目标方:     ConnectInvite
    MSG_GET_DEV_LIST_REQ      = 0x09,  // 设备列表查询:   DevListReq -> DevListRsp(+DevListEntry*n)
    MSG_GET_DEV_LIST_RSP      = 0x0A,
    MSG_GET_SERVER_LIST_REQ   = 0x0B,  // 服务器列表查询 -> ServerListRsp
    MSG_GET_SERVER_LIST_RSP   = 0x0C,
    MSG_ADD_UID_REQ           = 0x0D,  // 主动注册:       UuidReq
    MSG_DELETE_UID_REQ        = 0x0E,  // 注销:           UuidReq(uuid 有效)
    MSG_CHECK_UID_REQ         = 0x0F,  // 校验 UUID 存在 -> MSG_CHECK_UID_RSP
    MSG_CHECK_UID_RSP         = 0x12,  // 1 字节 result(0/1)
    MSG_SP_ASK_EXTINFO_REQ    = 0x10,  // NatServer -> Proxy 可用性查询: ProxyAvailReq
    MSG_SP_ASK_EXTINFO_RSP    = 0x11,  // Proxy -> NatServer: ProxyAvailRsp
    // ---- Proxy 侧 ----
    MSG_PROXY_REGISTER_REQ    = 0x20,  // 代理注册:       ProxyRegReq -> ProxyRegRsp
    MSG_PROXY_REGISTER_RSP    = 0x21,
    MSG_PROXY_UNREGISTER_REQ  = 0x22,  // 注销注册
    MSG_PROXY_RELAY_DATA      = 0x23,  // 中继数据:       RelayFrame + 业务负载
    MSG_PROXY_PUNCH_HELPER    = 0x24,  // 代理协助打洞:   RelayFrame(dst) -> ProxyRegRsp 返回 dst 地址
};

// -------------------------------------------------------------------------
// 业务负载结构体
// -------------------------------------------------------------------------

// 心跳 / 主动注册
struct UuidReq {
    char     uuid[MAX_UUID_LEN + 1];  // NUL 结尾
    uint8_t  dev_type;                // 0=未知 1=设备 2=App/客户端
    uint16_t lan_port;                // 本机 UDP 端口（网络序）
};

// 地址信息请求（仅需 uuid）
struct ExtInfoReq {
    char uuid[MAX_UUID_LEN + 1];
};

// 公网地址应答
struct ExtInfoRsp {
    char     pub_ip[MAX_IP_LEN];      // 服务端观察到的公网 IP
    uint16_t pub_port;                // 网络序
    char     lan_ip[MAX_IP_LEN];      // 上报的私网 IP
    uint16_t lan_port;                // 网络序
};

// 连接请求
struct ConnectReq {
    char src_uuid[MAX_UUID_LEN + 1];
    char dst_uuid[MAX_UUID_LEN + 1];
};

// 应答发起方：携带目标公网/私网地址与兜底代理
struct ConnectAck {
    uint8_t  result;                  // 0=成功 1=目标不在线 2=目标不存在
    char     dst_uuid[MAX_UUID_LEN + 1];
    char     dst_pub_ip[MAX_IP_LEN];
    uint16_t dst_pub_port;            // 网络序
    char     dst_lan_ip[MAX_IP_LEN];
    uint16_t dst_lan_port;            // 网络序
    char     proxy_ip[MAX_IP_LEN];    // 兜底中继代理
    uint16_t proxy_port;              // 网络序
};

// 通知目标方：携带发起方公网/私网地址与兜底代理
struct ConnectInvite {
    char     src_uuid[MAX_UUID_LEN + 1];
    char     src_pub_ip[MAX_IP_LEN];
    uint16_t src_pub_port;            // 网络序
    char     src_lan_ip[MAX_IP_LEN];
    uint16_t src_lan_port;            // 网络序
    char     proxy_ip[MAX_IP_LEN];
    uint16_t proxy_port;              // 网络序
};

// 设备列表查询
struct DevListReq {
    uint16_t start_index;             // 起始下标（网络序）
    uint16_t want_num;                // 期望条数（网络序）
};

// 设备列表应答（定长头 + DevListEntry * count）
struct DevListRsp {
    uint16_t total;                   // 当前在线总数（网络序）
    uint16_t count;                   // 本包条数（网络序）
    uint16_t start_index;             // 起始下标（网络序）
};

struct DevListEntry {
    char     uuid[MAX_UUID_LEN + 1];
    char     ip[MAX_IP_LEN];
    uint16_t port;                    // 网络序
    uint8_t  dev_type;
};

// 服务器列表应答（定长头 + nat_count + proxy_count 个 16 字节 IP 串）
struct ServerListRsp {
    uint16_t nat_count;               // 网络序
    uint16_t proxy_count;             // 网络序
};

// NatServer -> Proxy 可用性查询
struct ProxyAvailReq {
    char     nat_ip[MAX_IP_LEN];
    uint16_t nat_port;                // 网络序
};

// Proxy 可用性应答
struct ProxyAvailRsp {
    uint8_t  available;               // 0=忙 1=可用
    uint16_t used;                    // 已用代理数（网络序）
    uint16_t max_proxy;               // 上限（网络序）
};

// 代理注册
struct ProxyRegReq {
    char uuid[MAX_UUID_LEN + 1];
};

// 代理注册应答
struct ProxyRegRsp {
    uint8_t  result;                  // 0=成功 1=代理已满 2=参数错误
    char     pub_ip[MAX_IP_LEN];      // 观察到的公网地址
    uint16_t pub_port;                // 网络序
};

// 中继帧：源/目标 UUID 定长，其后为业务负载（如 PeerData）
struct RelayFrame {
    char src_uuid[MAX_UUID_LEN + 1];
    char dst_uuid[MAX_UUID_LEN + 1];
};

// 代理协助打洞应答（复用 ProxyRegRsp 返回 dst 公网地址）
// 提示：可通过代理获知对称 NAT 下目标的实际地址再本地打洞

// -------------------------------------------------------------------------
// 对端直连业务数据（打洞成功后的 P2P 通道，无报文头，直接裸发）
// -------------------------------------------------------------------------
struct PeerData {
    char     magic[4];                // "PDAT"
    uint8_t  type;                    // 0=ping 1=pong 2=punch
    uint8_t  seq;
    char     src_uuid[MAX_UUID_LEN + 1];
    char     msg[64];
};

#pragma pack(pop)

// 结果码
enum ConnectResult : uint8_t {
    CONNECT_OK        = 0,
    CONNECT_OFFLINE   = 1,
    CONNECT_NOT_FOUND = 2,
};

} // namespace p2p

#endif // P2P_PROTO_DEF_H
