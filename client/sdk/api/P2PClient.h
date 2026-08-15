#ifndef P2P_SDK_API_P2P_CLIENT_H
#define P2P_SDK_API_P2P_CLIENT_H

// P2PClient 门面：注册/心跳、NAT 检测、鉴权、CONNECT 打洞、中继兜底、
// 会话（逻辑通道 + 可靠 UDP）。单线程事件循环驱动，回调均在内部线程触发。

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/Crypto.h"
#include "common/ProtoDef.h"
#include "client/sdk/plat/Plat.h"
#include "client/sdk/proto/Codec.h"
#include "client/sdk/transport/NatDetect.h"
#include "client/sdk/transport/UdpSocket.h"
#include "client/sdk/session/Session.h"
#include "juice/juice.h"

namespace p2p {

class P2PClient {
public:
    struct ServerAddr {
        std::string ip;
        uint16_t    port;
    };

    struct Config {
        std::string uuid;                      // 本机 UUID（<=32 字符）
        std::string secret;                    // 鉴权密钥（空=不鉴权）
        std::vector<ServerAddr> nat_servers;   // NAT 服务器（取首个）
        std::vector<ServerAddr> proxy_servers; // 兜底代理（可空，用 ack/invite 的）
        uint32_t heartbeat_ms       = 20000;
        uint32_t heartbeat_retry_ms = 3000;
        uint32_t nat_detect_timeout_ms = 2500;
        uint32_t connect_timeout_ms = 6000;    // 打洞超时后降级中继
        uint32_t punch_interval_ms  = 1000;    // 打洞发包间隔
        uint32_t auth_timeout_ms    = 3000;
        uint32_t relay_register_ms  = 1000;    // 中继注册间隔
        bool nat_detect_enabled = true;
        bool auto_relay = true;                // 打洞失败自动降级中继
        bool force_relay = false;              // 跳过打洞，强制走中继（测试/合规场景）
    };

    // ---- 事件回调（在工作线程内触发，回调中勿阻塞/重入 stop） ----
    std::function<void(const char* pub_ip, uint16_t pub_port, uint8_t nattype)> on_ready;
    std::function<void(const std::string& peer, bool relay)> on_connected;
    std::function<void(const std::string& peer, uint8_t channel,
                       const uint8_t* data, size_t len)> on_message;
    std::function<void(const std::string& peer)> on_disconnected;
    std::function<void(const std::string& err)> on_error;

    P2PClient() = default;
    ~P2PClient() { stop(); }
    P2PClient(const P2PClient&) = delete;
    P2PClient& operator=(const P2PClient&) = delete;

    bool start(const Config& cfg);
    void stop();

    // 连接对端（异步：回调 on_connected 表示路径就绪）
    void connect(const std::string& peer_uuid);
    void disconnect(const std::string& peer_uuid);

    // 发送（reliable=true 走可靠通道；false 直接不可靠投递）
    int send(const std::string& peer_uuid, uint8_t channel,
             const void* data, size_t len, bool reliable = true);

    uint8_t  nat_type() const { return nat_type_; }
    uint16_t local_port() const { return sock_.local_port(); }
    const char* public_ip() const { return pub_ip_; }
    uint16_t public_port() const { return pub_port_; }
    bool running() const { return running_.load(); }
    uint64_t tunnel_enc_tx() const { return tunnel_enc_tx_.load(); }
    uint64_t tunnel_enc_rx() const { return tunnel_enc_rx_.load(); }

private:
    // 打洞子状态机（负责直连路径探测与超时）
    struct PunchState {
        sockaddr_in direct = {0};      // 打洞目标（公网）
        sockaddr_in direct_lan = {0};  // 同局域网直连目标（可选）
        bool have_direct = false;
        bool have_lan = false;
        bool direct_ok = false;
        uint64_t punch_deadline = 0;
        uint64_t next_punch = 0;
        std::vector<UdpSocket> punch_socks;  // 多 socket 打洞池（并行探测，提升穿透率）
        int direct_sock_idx = -1;            // 直连确认时选用的打洞 socket 索引（-1=未确认）
        juice_agent_t* juice = nullptr;      // #19 libjuice ICE agent（替换自研打洞）
        std::string local_sdp;               // #19 本端 ICE SDP（gather 后填充）
        std::string remote_sdp;              // #19 对端 ICE SDP（信令交换）
    };

    // 中继子状态机（负责 relay 注册与保活）
    struct RelayState {
        bool relay_ok = false;
        uint64_t relay_deadline = 0;
        uint64_t next_relay_ping = 0;
        std::vector<ServerAddr> proxies;
    };

    // 连接主状态机（组合打洞/中继子状态，负责整体连接生命周期）
    struct Conn {
        std::string peer_uuid;
        PunchState punch;
        RelayState relay;
        bool connecting = false;
        bool connected = false;
        uint32_t backoff_attempt = 0;   // #18 连接级退避尝试计数（CONNECT 重发/中继注册）
        P2PClient* self = nullptr;      // #19 反向指针，供 libjuice 回调触发 on_connected
    };

    // #18 统一指数退避辅助结构
    struct BackOff {
        uint32_t attempt = 0;
        uint32_t base_ms = 1000;
        uint32_t cap_ms = 30000;
        uint32_t next_delay() {
            uint32_t d = base_ms;
            for (uint32_t i = 0; i + 1 < attempt && d < cap_ms; i++) d *= 2;
            if (d > cap_ms) d = cap_ms;
            attempt++;
            return d;
        }
        void reset() { attempt = 0; }
    };

    // ---- 内部 ----
    void worker_loop();
    void tick(uint64_t now);
    // ---- 状态机分阶段驱动（tick 内部按序调用，各自负责单一职责） ----
    void tick_heartbeat(uint64_t now);       // 心跳发送与重试间隔
    void tick_auth(uint64_t now);            // 鉴权状态机（challenge/login 重试）
    void tick_nat_detect(uint64_t now);      // NAT 检测推进
    void tick_relay(uint64_t now);           // 中继注册重试
    void tick_connections(uint64_t now);     // 连接状态机（打洞/超时/降级中继）
    void tick_conn_punch(Conn& c, uint64_t now);   // 打洞子状态机
    void tick_conn_relay(Conn& c, uint64_t now);   // 中继子状态机
    void tick_conn_fsm(Conn& c, uint64_t now);     // 连接主状态机（中继建链判定）
    void tick_sessions(uint64_t now);        // 会话周期驱动
    void handle_packet(const uint8_t* buf, size_t len, const sockaddr_in& from);
    void handle_proto(uint8_t msg_id, const uint8_t* p, size_t plen,
                      const sockaddr_in& from);
    void send_proto(uint8_t msg_id, const void* payload, size_t plen,
                    const sockaddr_in& to);
    void send_proto(uint8_t msg_id, const void* payload, size_t plen); // -> NAT 服务器
    void do_heartbeat();
    void on_heartbeat_rsp(const uint8_t* p, size_t plen);
    void on_heartbeat_rsp_enc(const uint8_t* p, size_t plen);
    void do_auth_challenge();
    void do_auth_login();
    void on_auth_challenge_rsp(const uint8_t* p, size_t plen);
    void on_auth_login_rsp(const uint8_t* p, size_t plen);
    void on_nat_detect_rsp(const uint8_t* p, size_t plen, bool from_alt_req);
    void on_connect_ack(const uint8_t* p, size_t plen);
    void on_connect_invite(const uint8_t* p, size_t plen);
    void on_proxy_register_rsp(const uint8_t* p, size_t plen);
    void on_proxy_relay_data(const uint8_t* p, size_t plen);
    void ensure_punch_pool(Conn& c);
    // #19 阶段 B 收尾：经 NatServer 发送本地 ICE SDP / 接收对端 SDP
    void send_ice_sdp(const std::string& peer, const std::string& local_sdp);
    void on_ice_sdp(const uint8_t* p, size_t plen);
    void ensure_ice_agent(Conn& c);          // 发起方/被邀方统一创建 juice agent
    void apply_remote_ice_sdp(Conn& c, const std::string& remote_sdp);
    void do_punch(Conn& c);
    // #19 libjuice ICE 回调（静态转发至 Conn 上下文）
    static void on_juice_state(juice_agent_t* agent, juice_state_t state, void* user_ptr);
    static void on_juice_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr);
    static void on_juice_gathering_done(juice_agent_t* agent, void* user_ptr);
    static void on_juice_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr);
    void relay_register();
    void relay_unregister_all();
    void send_tunnel_via(Conn& c, const uint8_t* frame, size_t len);
    void on_tunnel_frame(const uint8_t* frame, size_t len,
                         const std::string& peer_uuid);
    Session* session_for(const std::string& peer);
    void close_conn(Conn& c);

    // ---- 隧道负载加密（鉴权开启时对端间共享密钥派生） ----
    bool get_tunnel_key(const std::string& peer, uint8_t out[32]);
    bool encrypt_tunnel_frame(const std::string& peer, uint8_t* out,
                              const uint8_t* in, size_t inlen, size_t* outlen);
    bool decrypt_tunnel_frame(const std::string& peer, uint8_t* out,
                              const uint8_t* in, size_t inlen, size_t* outlen);

    bool parse_proxies(uint8_t count, const ProxyCandidate* cands,
                       std::vector<ServerAddr>& out);

    UdpSocket sock_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> worker_done_{false};
    Config cfg_;
    std::string secret_;

    // 服务器状态
    ServerAddr nat_server_;
    sockaddr_in nat_sock_;
    uint16_t   alt_port_ = 0;
    char       pub_ip_[16] = {0};
    uint16_t   pub_port_ = 0;
    uint64_t   next_heartbeat_ms_ = 0;
    uint32_t   heartbeat_fail_ = 0;
    BackOff heartbeat_backoff_;      // #18 心跳失败指数退避

    // 鉴权状态
    bool   authed_ = true;
    bool   heartbeat_enc_ = false;   // 鉴权后心跳走加密通道（MSG_HEARTBEAT_REQ_ENC）
    bool   auth_denied_ = false;     // 永久性拒绝（白名单/黑名单/MAC），不再重试
    uint8_t auth_nonce_[16] = {0};
    uint64_t next_auth_try_ = 0;
    bool   auth_inflight_ = false;
    BackOff auth_backoff_;          // #18 鉴权失败指数退避

    // 隧道负载加密
    bool tunnel_enc_ = false;   // secret 非空即开启（需对端同密钥）
    std::unordered_map<std::string, std::array<uint8_t, 32>> tunnel_keys_;
    std::atomic<uint64_t> tunnel_enc_tx_{0};
    std::atomic<uint64_t> tunnel_enc_rx_{0};

    // NAT 检测
    NatDetect nat_detect_;
    uint8_t  nat_type_ = NAT_UNKNOWN;

    // 中继状态
    std::vector<ServerAddr> proxies_;
    uint64_t next_relay_reg_ = 0;
    bool     relay_registered_ = false;

    // 会话与连接
    std::recursive_mutex mu_;
    std::unordered_map<std::string, Conn> conns_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<uint64_t, std::string> addr_to_peer_;   // ip<<16|port -> peer uuid
    uint16_t next_session_id_ = 1;
    bool ready_fired_ = false;
    // ICE_SDP 可能早于 CONNECT_INVITE 到达（uuid 字段是 dst）；暂存待邀方创建后再应用
    std::string pending_remote_sdp_;

    uint8_t recv_buf_[MAX_PKT];
};

} // namespace p2p

#endif // P2P_SDK_API_P2P_CLIENT_H
