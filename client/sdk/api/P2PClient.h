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
#include "common/Handshake.h"
#include "common/ProtoDef.h"
#include "common/Uid.h"
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
        std::string uuid;                      // 本机 UUID（<=32 字符；UidStrict 服务端须为 20 字符结构化 UID）
        std::string secret;                    // 主密钥（运维/演示用；空=见 auth_key_hex）
        std::string auth_key_hex;              // 每 UID 独立 AuthKey（64 hex，uidgen 签发；
                                               //   设备侧推荐只烧录此项，不知晓主密钥）
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
        std::string connect_token_hex;         // 默认连线 Token（104 hex；也可在 connect() 传入）
        bool lan_discover = true;              // 局域网组播发现（对标 TUTK LAN Search）
    };

    struct LanPeer {
        std::string uuid;
        std::string ip;
        uint16_t    port = 0;
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
    // token_hex 非空则覆盖 Config.connect_token_hex（EnableConnectToken 服务端必带）
    void connect(const std::string& peer_uuid, const std::string& token_hex = {});
    void disconnect(const std::string& peer_uuid);
    // ICE restart：换网/路径劣化时重建 agent（juice 不支持原地换 ufrag）
    void restart_ice(const std::string& peer_uuid);
    uint64_t ice_restarts() const { return ice_restarts_.load(); }

    // 发送（reliable=true 走可靠通道；false 直接不可靠投递）
    int send(const std::string& peer_uuid, uint8_t channel,
             const void* data, size_t len, bool reliable = true);

    // 链路统计（RTT/丢包/重传/FEC），供上层自适应码率；对端会话不存在返回 false
    bool link_stats(const std::string& peer_uuid, Session::LinkStats& out);

    // 局域网发现缓存（15s 内听到的 ANNOUNCE）；供 IOTC_Search_Device
    std::vector<LanPeer> lan_peers();
    bool lan_lookup(const std::string& uuid, LanPeer& out);

    uint8_t  nat_type() const { return nat_type_; }
    uint16_t local_port() const { return sock_.local_port(); }
    const char* public_ip() const { return pub_ip_; }
    uint16_t public_port() const { return pub_port_; }
    bool running() const { return running_.load(); }
    uint64_t tunnel_enc_tx() const { return tunnel_enc_tx_.load(); }
    uint64_t tunnel_enc_rx() const { return tunnel_enc_rx_.load(); }
    uint64_t tunnel_fs_ok() const { return tunnel_fs_ok_.load(); }

private:
    // 打洞子状态机（负责直连路径探测与超时）
    struct PunchState {
        sockaddr_in direct{};          // 打洞目标（公网）
        sockaddr_in direct_lan{};      // 同局域网直连目标（可选）
        bool have_direct = false;
        bool have_lan = false;
        bool direct_ok = false;
        uint64_t punch_deadline = 0;
        uint64_t next_punch = 0;
        std::vector<UdpSocket> punch_socks;  // 多 socket 打洞池（并行探测，提升穿透率）
        int direct_sock_idx = -1;            // 直连确认时选用的打洞 socket 索引（-1=未确认）
        juice_agent_t* juice = nullptr;      // #19 libjuice ICE agent（替换自研打洞）
        juice_agent_t* juice_prev = nullptr; // restart 期间旧 agent，继续扛媒体
        bool ice_gathered = false;           // 已 juice_gather：发起方先 gather=controlling
        bool ice_host_sdp_sent = false;      // 已用 host 候选发出首版 SDP（不等 STUN）
        bool ice_remote_gather_done = false; // 已通知 juice 对端收集结束
        bool ice_restarting = false;         // 正在换代，tick_ice 仍要重传 SDP
        bool ice_nominated = false;          // 当前 juice 已 CONNECTED，可 juice_send
        uint8_t  ice_gen = 0;                // restart 代数（日志）
        uint64_t ice_remote_applied_ms = 0;  // 首次 set_remote 时间，供延迟标记 gathering done
        uint64_t ice_sdp_rtx_ms = 0;         // 最近一次发出本地 SDP
        uint8_t  ice_sdp_rtx_n = 0;          // 连线中 SDP 重传次数
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

    // 连接生命周期状态（转移：Idle → Connecting → Connected；close_conn 直接移除条目）
    enum class ConnState : uint8_t {
        Idle,        // 条目刚创建，尚未发起
        Connecting,  // 已发起，等待直连打洞或中继路径就绪
        Connected,   // 任一路径就绪（路径类型见 via_relay）
    };

    // 连接主状态机（组合打洞/中继子状态，负责整体连接生命周期）
    struct Conn {
        std::string peer_uuid;
        PunchState punch;
        RelayState relay;
        ConnState state = ConnState::Idle;
        bool via_relay = false;         // Connected 时的路径类型（true=中继）
        std::string connect_token_hex;  // 本连接出示的连线 Token（可空）
        uint32_t backoff_attempt = 0;   // #18 连接级退避尝试计数（CONNECT 重发/中继注册）
        P2PClient* self = nullptr;      // #19 反向指针，供 libjuice 回调触发 on_connected

        bool connecting() const { return state == ConnState::Connecting; }
        bool connected() const { return state == ConnState::Connected; }
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
    void tick_connections(uint64_t now);     // 连接状态机（打洞/超时/降级中继/回切 P2P）
    void tick_conn_punch(Conn& c, uint64_t now);   // 打洞子状态机
    void tick_conn_relay(Conn& c, uint64_t now);   // 中继子状态机
    void tick_conn_fsm(Conn& c, uint64_t now);     // 连接主状态机（中继建链判定）
    void tick_sessions(uint64_t now);        // 会话周期驱动
    void tick_lan(uint64_t now);             // 局域网组播宣告 / 缓存过期
    void tick_ice(uint64_t now);             // 延迟标记 remote gathering done
    void handle_packet(const uint8_t* buf, size_t len, const sockaddr_in& from);
    void handle_proto(uint8_t msg_id, const uint8_t* p, size_t plen,
                      const sockaddr_in& from);
    void send_proto(uint8_t msg_id, const void* payload, size_t plen,
                    const sockaddr_in& to);
    void send_proto(uint8_t msg_id, const void* payload, size_t plen); // -> NAT 服务器
    void send_connect_req(const Conn& c);
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
    void ensure_ice_agent(Conn& c, bool as_offerer);  // offerer=true 才 gather（controlling）
    void start_ice_gather(Conn& c);
    void apply_remote_ice_sdp(Conn& c, const std::string& remote_sdp);
    void begin_ice_restart(Conn& c, bool as_offerer);
    void reset_ice_flags(Conn& c);
    void reap_juice(juice_agent_t*& agent);
    void send_lan_beacon(uint8_t msg_id, const std::string& uuid, const sockaddr_in* to);
    void on_lan_beacon(uint8_t msg_id, const uint8_t* p, size_t plen,
                       const sockaddr_in& from);
    void apply_lan_peer(const std::string& uuid, const sockaddr_in& from,
                        uint16_t media_port_nbo);
    void remember_lan_peer(const std::string& uuid, const sockaddr_in& from,
                           uint16_t media_port_nbo);
    Conn* conn_of(const std::string& uuid);
    Conn& ensure_conn(const std::string& uuid);
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
    // 连接建立唯一转移点（幂等）：置 Connected 并触发 on_connected
    void set_connected(Conn& c, bool relay);

    // P5：X25519 握手（可靠通道 0xFE）
    void start_handshake(const std::string& peer);
    void on_hs_msg(const std::string& peer, const uint8_t* data, size_t len);
    void finish_handshake(const std::string& peer);
    void tick_handshake(uint64_t now);
    bool psk_tunnel_key(const std::string& peer, uint8_t out[32]);

    // ---- 隧道负载加密（PSK 或 ECDH 会话密钥） ----
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
    bool   has_cred_ = false;        // 持有鉴权凭据（secret 或 auth_key 任一）
    uint8_t auth_key_[32] = {0};     // 每 UID 独立 AuthKey（P1：配置或从 secret 派生）
    bool   authed_ = true;
    bool   heartbeat_enc_ = false;   // 鉴权后心跳走加密通道（MSG_HEARTBEAT_REQ_ENC）
    bool   auth_denied_ = false;     // 永久性拒绝（白名单/黑名单/MAC），不再重试
    uint8_t auth_nonce_[16] = {0};
    uint64_t next_auth_try_ = 0;
    bool   auth_inflight_ = false;
    BackOff auth_backoff_;          // #18 鉴权失败指数退避

    // 隧道负载加密
    bool tunnel_enc_ = false;   // PSK 或握手完成后开启
    std::unordered_map<std::string, std::array<uint8_t, 32>> tunnel_keys_;
    std::atomic<uint64_t> tunnel_enc_tx_{0};
    std::atomic<uint64_t> tunnel_enc_rx_{0};
    std::atomic<uint64_t> tunnel_fs_ok_{0};
    std::atomic<uint64_t> ice_restarts_{0};

    struct HsState {
        uint8_t priv[32]{};
        uint8_t pub[32]{};
        uint8_t nonce[16]{};
        uint8_t peer_pub[32]{};
        uint8_t peer_nonce[16]{};
        bool local_ready = false;
        bool remote_ready = false;
        bool done = false;
        bool peer_acked = false;   // 对端已派生，可以开始用 FS 密钥加密
        uint64_t last_rekey_ms = 0;
    };
    struct FsKeyPair {
        std::array<uint8_t, 32> cur{};
        std::array<uint8_t, 32> prev{};
        bool has_cur = false;
        bool has_prev = false;
    };
    std::unordered_map<std::string, HsState> hs_;
    std::unordered_map<std::string, FsKeyPair> fs_keys_;

    // NAT 检测
    NatDetect nat_detect_;
    uint8_t  nat_type_ = NAT_UNKNOWN;

    // 中继状态
    std::vector<ServerAddr> proxies_;
    uint64_t next_relay_reg_ = 0;
    bool     relay_registered_ = false;

    // 会话与连接
    std::recursive_mutex mu_;
    // unique_ptr：juice user_ptr 指向 Conn，map 扩容不得移动对象
    std::unordered_map<std::string, std::unique_ptr<Conn>> conns_;
    std::vector<juice_agent_t*> juice_reap_; // close_conn 摘下，锁外销毁
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<uint64_t, std::string> addr_to_peer_;   // ip<<16|port -> peer uuid
    uint16_t next_session_id_ = 1;
    bool ready_fired_ = false;
    // ICE_SDP 可能早于 CONNECT_INVITE：按 src uuid 暂存，邀方创建后再应用
    std::unordered_map<std::string, std::string> pending_remote_sdp_;

    // 局域网发现
    UdpSocket lan_sock_;
    bool lan_enabled_ = false;
    uint64_t next_lan_announce_ms_ = 0;
    struct LanCacheEnt {
        sockaddr_in addr{};
        uint64_t seen_ms = 0;
    };
    std::unordered_map<std::string, LanCacheEnt> lan_cache_;

    uint8_t recv_buf_[MAX_PKT];
};

} // namespace p2p

#endif // P2P_SDK_API_P2P_CLIENT_H
