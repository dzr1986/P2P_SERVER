#ifndef P2P_SDK_IOTC_IOTC_H
#define P2P_SDK_IOTC_IOTC_H

// IOTC 会话层（计划书 P2）：C 风格导出 API，对标 TUTK IOTCAPIs 使用习惯
//   - 进程级单例：Initialize -> Login(UID 报到) -> Connect_ByUID / Listen
//   - SID 句柄表（上限 128），会话内 32 条逻辑通道（0~31）
//   - 通道读写：Write 直发（可靠/不可靠可选），Read 阻塞出队（轮询模式）
//   - 事件回调模式仍可直接使用底层 P2PClient（api/P2PClient.h）
// 线程模型：内部持有一个 P2PClient 工作线程；所有 API 线程安全。

#include <cstdint>

extern "C" {

// ---- 常量 ----
enum {
    IOTC_MAX_SESSIONS = 128,   // 对标 TUTK IOTCAPIs 最大连接数
    IOTC_MAX_CHANNELS = 32,    // 对标 TUTK 每连接最大通道数
};

// ---- 错误码（<0）----
enum {
    IOTC_ER_NoERROR            = 0,
    IOTC_ER_NotInitialized     = -1,
    IOTC_ER_AlreadyInitialized = -2,
    IOTC_ER_InvalidArg         = -3,
    IOTC_ER_NotLoggedIn        = -4,
    IOTC_ER_LoginTimeout       = -5,
    IOTC_ER_ExceedMaxSession   = -6,
    IOTC_ER_SessionNoExist     = -7,
    IOTC_ER_SessionClosed      = -8,
    IOTC_ER_ConnectTimeout     = -9,
    IOTC_ER_Timeout            = -10,
    IOTC_ER_BufferTooSmall     = -11,
    IOTC_ER_SendFail           = -12,
};

// 初始化：设置 P2P 服务器地址（NatServer）。重复调用返回 AlreadyInitialized。
int  IOTC_Initialize(const char* server_ip, uint16_t server_port);
void IOTC_DeInitialize(void);

// 报到登录（设备端与客户端同用）：
//   uid          本端 UID（UidStrict 服务端须为 20 字符结构化 UID）
//   secret       主密钥（运维/演示；可为 NULL）
//   auth_key_hex 每 UID AuthKey 64hex（设备侧凭据，可为 NULL；与 secret 至少给一个或都不给=免鉴权）
//   timeout_ms   等待注册完成（on_ready）的超时
// 返回 0 / 错误码
int  IOTC_Login(const char* uid, const char* secret, const char* auth_key_hex,
                int timeout_ms);

// 可选：在 Login 前设置中继与强制中继（P4 中继验收 / 弱网兜底）
int  IOTC_SetProxy(const char* proxy_ip, uint16_t proxy_port);
void IOTC_ForceRelay(int enable);
// 下次 IOTC_Connect_ByUID 出示的连线 Token（104 hex；NULL/空串=不带）
int  IOTC_SetConnectToken(const char* token_hex);

// 客户端：连接目标 UID，阻塞直至路径就绪（直连或中继），返回 SID(>=0) 或错误码
int  IOTC_Connect_ByUID(const char* peer_uid, int timeout_ms);

// 设备端：等待入站连接，返回新会话 SID(>=0) 或 IOTC_ER_Timeout
int  IOTC_Listen(int timeout_ms);

// 关闭会话（本端释放句柄并通知对端）
int  IOTC_Session_Close(int sid);

// 会话内通道写：reliable=1 可靠（重传/有序），0 不可靠（低延迟 + FEC）
//   返回 0 / 错误码（可靠窗口满重试超时返回 IOTC_ER_SendFail，上层应降码率）
int  IOTC_Session_Write(int sid, uint8_t channel, const void* data, int len,
                        int reliable);

// 会话内通道读：阻塞至多 timeout_ms，返回消息字节数(>0) 或错误码
int  IOTC_Session_Read(int sid, uint8_t channel, void* buf, int cap,
                       int timeout_ms);

// 会话信息
typedef struct {
    char peer_uid[33];
    int  connected;    // 1=路径就绪
    int  via_relay;    // 1=中继路径
} IOTCSessionInfo;
int  IOTC_Session_Check(int sid, IOTCSessionInfo* info);

// 链路统计（供 AV 自适应码率；对标 Session::LinkStats）
typedef struct {
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    uint32_t rto_ms;
    uint32_t cwnd;
    uint32_t inflight;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t retrans;
    uint64_t fast_retrans;
    uint64_t fec_sent;
    uint64_t fec_recovered;
    uint64_t rx_lost;
} IOTCLinkStats;
int  IOTC_Session_GetLinkStats(int sid, IOTCLinkStats* st);

// 局域网发现（对标 TUTK IOTC_Search_Device）：等待 timeout_ms 收集 ANNOUNCE
// 返回写入条数(>=0) 或错误码
typedef struct {
    char     uuid[33];
    char     ip[16];
    uint16_t port;
} IOTCLanDevice;
int  IOTC_Search_Device(IOTCLanDevice* out, int cap, int timeout_ms);

} // extern "C"

#endif // P2P_SDK_IOTC_IOTC_H
