// IOTC 会话层实现（计划书 P2）
//   进程级单例包装 P2PClient：SID 句柄表 + 每会话每通道接收队列 + 阻塞读
// 锁序约定：绝不在持有 g_mu 时调用 P2PClient 方法（其回调线程会反向取 g_mu，防 ABBA 死锁）
#include "IOTC.h"

#include "client/sdk/api/P2PClient.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace p2p;

constexpr size_t kChannelQueueCap = 1024;   // 每通道积压上限（超限丢最旧，防内存膨胀）

struct SessionSlot {
    bool used = false;
    bool connected = false;
    bool closed = false;
    bool via_relay = false;
    std::string peer;
    std::deque<std::vector<uint8_t>> ch[IOTC_MAX_CHANNELS];
};

struct IotcCtx {
    std::mutex mu;
    std::condition_variable cv;
    bool logged_in = false;
    bool ready = false;
    std::string server_ip;
    uint16_t server_port = 0;
    P2PClient client;
    SessionSlot sess[IOTC_MAX_SESSIONS];
    std::unordered_map<std::string, int> peer2sid;
    std::deque<int> incoming;   // 入站会话待 IOTC_Listen 取走
};

std::unique_ptr<IotcCtx> g;
std::mutex g_life_mu;   // 保护 g 本身的创建/销毁

// 需持有 g->mu 调用
int alloc_sid_locked(IotcCtx& c, const std::string& peer) {
    for (int i = 0; i < IOTC_MAX_SESSIONS; i++) {
        if (!c.sess[i].used) {
            c.sess[i] = SessionSlot{};
            c.sess[i].used = true;
            c.sess[i].peer = peer;
            c.peer2sid[peer] = i;
            return i;
        }
    }
    return IOTC_ER_ExceedMaxSession;
}

void wire_callbacks(IotcCtx& c) {
    c.client.on_ready = [&c](const char*, uint16_t, uint8_t) {
        std::lock_guard<std::mutex> lk(c.mu);
        c.ready = true;
        c.cv.notify_all();
    };
    c.client.on_connected = [&c](const std::string& peer, bool relay) {
        std::lock_guard<std::mutex> lk(c.mu);
        auto it = c.peer2sid.find(peer);
        int sid;
        if (it != c.peer2sid.end()) {
            sid = it->second;                 // 本端发起，Connect 正在等待
        } else {
            sid = alloc_sid_locked(c, peer);  // 对端连入
            if (sid < 0) return;              // 句柄耗尽：忽略（对端会超时）
            c.incoming.push_back(sid);
        }
        c.sess[sid].connected = true;
        c.sess[sid].via_relay = relay;
        c.cv.notify_all();
    };
    c.client.on_message = [&c](const std::string& peer, uint8_t ch,
                               const uint8_t* data, size_t len) {
        if (ch >= IOTC_MAX_CHANNELS) return;
        std::lock_guard<std::mutex> lk(c.mu);
        auto it = c.peer2sid.find(peer);
        if (it == c.peer2sid.end()) return;
        auto& q = c.sess[it->second].ch[ch];
        if (q.size() >= kChannelQueueCap) q.pop_front();
        q.emplace_back(data, data + len);
        c.cv.notify_all();
    };
    c.client.on_disconnected = [&c](const std::string& peer) {
        std::lock_guard<std::mutex> lk(c.mu);
        auto it = c.peer2sid.find(peer);
        if (it == c.peer2sid.end()) return;
        c.sess[it->second].closed = true;
        c.sess[it->second].connected = false;
        c.cv.notify_all();
    };
}

} // namespace

extern "C" {

int IOTC_Initialize(const char* server_ip, uint16_t server_port) {
    if (!server_ip || server_port == 0) return IOTC_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_life_mu);
    if (g) return IOTC_ER_AlreadyInitialized;
    g = std::make_unique<IotcCtx>();
    g->server_ip = server_ip;
    g->server_port = server_port;
    wire_callbacks(*g);
    return IOTC_ER_NoERROR;
}

void IOTC_DeInitialize(void) {
    std::lock_guard<std::mutex> lk(g_life_mu);
    if (!g) return;
    g->client.stop();
    g.reset();
}

int IOTC_Login(const char* uid, const char* secret, const char* auth_key_hex,
               int timeout_ms) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!uid || !uid[0]) return IOTC_ER_InvalidArg;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (g->logged_in) return IOTC_ER_AlreadyInitialized;
    }
    P2PClient::Config cfg;
    cfg.uuid = uid;
    if (secret) cfg.secret = secret;
    if (auth_key_hex) cfg.auth_key_hex = auth_key_hex;
    cfg.nat_servers.push_back({g->server_ip, g->server_port});
    if (!g->client.start(cfg)) return IOTC_ER_InvalidArg;

    std::unique_lock<std::mutex> lk(g->mu);
    g->logged_in = true;
    if (!g->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [] { return g->ready; }))
        return IOTC_ER_LoginTimeout;
    return IOTC_ER_NoERROR;
}

int IOTC_Connect_ByUID(const char* peer_uid, int timeout_ms) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!peer_uid || !peer_uid[0]) return IOTC_ER_InvalidArg;
    int sid;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (!g->logged_in) return IOTC_ER_NotLoggedIn;
        auto it = g->peer2sid.find(peer_uid);
        if (it != g->peer2sid.end()) {
            if (g->sess[it->second].connected) return it->second;   // 幂等
            sid = it->second;
        } else {
            sid = alloc_sid_locked(*g, peer_uid);
            if (sid < 0) return sid;
        }
    }
    g->client.connect(peer_uid);   // 锁外调用（见文件头锁序约定）

    std::unique_lock<std::mutex> lk(g->mu);
    bool ok = g->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [sid] { return g->sess[sid].connected ||
                                            g->sess[sid].closed; });
    if (!ok || !g->sess[sid].connected) {
        // 超时/失败：释放句柄（底层 Conn 由 P2PClient 超时逻辑自行回收）
        g->peer2sid.erase(g->sess[sid].peer);
        g->sess[sid] = SessionSlot{};
        return IOTC_ER_ConnectTimeout;
    }
    return sid;
}

int IOTC_Listen(int timeout_ms) {
    if (!g) return IOTC_ER_NotInitialized;
    std::unique_lock<std::mutex> lk(g->mu);
    if (!g->logged_in) return IOTC_ER_NotLoggedIn;
    if (!g->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [] { return !g->incoming.empty(); }))
        return IOTC_ER_Timeout;
    int sid = g->incoming.front();
    g->incoming.pop_front();
    return sid;
}

int IOTC_Session_Close(int sid) {
    if (!g) return IOTC_ER_NotInitialized;
    std::string peer;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (sid < 0 || sid >= IOTC_MAX_SESSIONS || !g->sess[sid].used)
            return IOTC_ER_SessionNoExist;
        peer = g->sess[sid].peer;
        g->peer2sid.erase(peer);
        g->sess[sid] = SessionSlot{};
    }
    g->client.disconnect(peer);   // 锁外
    return IOTC_ER_NoERROR;
}

int IOTC_Session_Write(int sid, uint8_t channel, const void* data, int len,
                       int reliable) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!data || len <= 0 || channel >= IOTC_MAX_CHANNELS) return IOTC_ER_InvalidArg;
    std::string peer;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (sid < 0 || sid >= IOTC_MAX_SESSIONS || !g->sess[sid].used)
            return IOTC_ER_SessionNoExist;
        if (g->sess[sid].closed) return IOTC_ER_SessionClosed;
        peer = g->sess[sid].peer;
    }
    // 可靠通道满窗（拥塞限流）时短暂重试；持续满窗即返回 SendFail（上层降码率信号）
    for (int i = 0; i < 20; i++) {
        int r = g->client.send(peer, channel, data, (size_t)len, reliable != 0);
        if (r == 0) return IOTC_ER_NoERROR;
        if (r != -2 || reliable == 0) return IOTC_ER_SendFail;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return IOTC_ER_SendFail;
}

int IOTC_Session_Read(int sid, uint8_t channel, void* buf, int cap,
                      int timeout_ms) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!buf || cap <= 0 || channel >= IOTC_MAX_CHANNELS) return IOTC_ER_InvalidArg;
    std::unique_lock<std::mutex> lk(g->mu);
    if (sid < 0 || sid >= IOTC_MAX_SESSIONS || !g->sess[sid].used)
        return IOTC_ER_SessionNoExist;
    auto& s = g->sess[sid];
    if (!g->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&s, channel] { return !s.ch[channel].empty() || s.closed; })) {
        return IOTC_ER_Timeout;
    }
    if (s.ch[channel].empty()) return IOTC_ER_SessionClosed;
    auto& msg = s.ch[channel].front();
    if ((int)msg.size() > cap) return IOTC_ER_BufferTooSmall;   // 不出队，可换大缓冲重读
    const int n = (int)msg.size();
    memcpy(buf, msg.data(), msg.size());
    s.ch[channel].pop_front();
    return n;
}

int IOTC_Session_Check(int sid, IOTCSessionInfo* info) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!info) return IOTC_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g->mu);
    if (sid < 0 || sid >= IOTC_MAX_SESSIONS || !g->sess[sid].used)
        return IOTC_ER_SessionNoExist;
    const auto& s = g->sess[sid];
    memset(info, 0, sizeof(*info));
    strncpy(info->peer_uid, s.peer.c_str(), sizeof(info->peer_uid) - 1);
    info->connected = s.connected ? 1 : 0;
    info->via_relay = s.via_relay ? 1 : 0;
    return IOTC_ER_NoERROR;
}

int IOTC_Session_GetLinkStats(int sid, IOTCLinkStats* st) {
    if (!g) return IOTC_ER_NotInitialized;
    if (!st) return IOTC_ER_InvalidArg;
    std::string peer;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (sid < 0 || sid >= IOTC_MAX_SESSIONS || !g->sess[sid].used)
            return IOTC_ER_SessionNoExist;
        peer = g->sess[sid].peer;
    }
    p2p::Session::LinkStats ls;
    if (!g->client.link_stats(peer, ls)) return IOTC_ER_SessionNoExist;
    st->srtt_ms = ls.srtt_ms;
    st->rttvar_ms = ls.rttvar_ms;
    st->rto_ms = ls.rto_ms;
    st->cwnd = ls.cwnd;
    st->inflight = ls.inflight;
    st->tx_bytes = ls.tx_bytes;
    st->rx_bytes = ls.rx_bytes;
    st->retrans = ls.retrans;
    st->fast_retrans = ls.fast_retrans;
    st->fec_sent = ls.fec_sent;
    st->fec_recovered = ls.fec_recovered;
    st->rx_lost = ls.rx_lost;
    return IOTC_ER_NoERROR;
}

} // extern "C"
