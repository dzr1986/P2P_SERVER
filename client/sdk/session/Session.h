#ifndef P2P_SDK_SESSION_SESSION_H
#define P2P_SDK_SESSION_SESSION_H

// 对端隧道会话：逻辑通道 + 可靠 UDP（seq/ack/重传/乱序缓冲）
// 与传输解耦：会话不感知打洞直连或中继，产出完整 TunnelFrame 字节流交由传输层投递

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "common/ProtoDef.h"
#include "client/sdk/plat/Plat.h"
#include "client/sdk/proto/Codec.h"

namespace p2p {

class Session {
public:
    struct Msg {
        uint16_t  seq;
        uint8_t   channel_id;
        uint8_t   flags;
        std::vector<uint8_t> data;   // 完整 TunnelFrame + 负载（直接发送）
        uint64_t  sent_ms;
    };

    // 交付回调：业务数据（channel, payload, len）
    std::function<void(uint8_t, const uint8_t*, size_t)> on_data;
    // 发送回调：完整隧道帧字节，交由传输层（直连/中继）
    std::function<void(const uint8_t*, size_t)> on_tx;

    explicit Session(uint16_t session_id) : session_id_(session_id) {}

    uint16_t id() const { return session_id_; }
    bool active() const { return active_; }
    void deactivate() { active_ = false; }

    // ---- 发送 ----
    // 返回 0 成功，-1 通道不可靠参数错误，-2 可靠窗口已满
    int send(uint8_t channel_id, const void* data, size_t len, bool reliable) {
        if (len > MAX_TUNNEL_PAYLOAD) return -1;
        const uint8_t* p = (const uint8_t*)data;
        uint16_t seq = 0;
        uint8_t  flags = 0;
        if (reliable) {
            flags |= TF_RELIABLE;
            if (pending_.size() >= 64) return -2;   // 窗口满
            seq = next_seq_++;
        }
        std::vector<uint8_t> frame(14 + len);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_DATA, session_id_,
                           channel_id, flags, seq, 0, p, (uint16_t)len);
        if (reliable) {
            Msg m;
            m.seq = seq;
            m.channel_id = channel_id;
            m.flags = flags;
            m.data = frame;
            m.sent_ms = plat_now_ms();
            pending_.push_back(std::move(m));
        }
        if (on_tx) on_tx(frame.data(), frame.size());
        return 0;
    }

    // ---- 接收（输入完整隧道帧字节） ----
    void on_frame(const uint8_t* buf, size_t len) {
        WireTunnel t;
        if (!codec_read_tunnel(buf, len, t)) return;
        switch (t.type) {
        case TT_DATA:   handle_data(t);  break;
        case TT_ACK:    handle_ack(t.ack); break;
        case TT_PING:   reply_pong(); break;
        case TT_CLOSE:  active_ = false; break;
        case TT_PONG:   break;  // 保活确认，无需处理
        default:        break;
        }
    }

    // 周期驱动：重传超时未确认帧 + 通道保活
    void tick(uint64_t now) {
        if (now - last_ping_ms_ >= 15000) {   // 隧道级保活
            last_ping_ms_ = now;
            std::vector<uint8_t> frame(14);
            codec_write_tunnel(frame.data(), (int)frame.size(), TT_PING, session_id_,
                               0, 0, 0, 0, nullptr, 0);
            if (on_tx) on_tx(frame.data(), frame.size());
        }
        for (auto& m : pending_) {
            if (now - m.sent_ms >= rto_ms_) {
                if (on_tx) on_tx(m.data.data(), m.data.size());
                m.sent_ms = now;
            }
        }
    }

    // 统计
    size_t pending_size() const { return pending_.size(); }
    uint64_t tx_bytes() const { return tx_bytes_; }
    uint64_t rx_bytes() const { return rx_bytes_; }

private:
    void handle_data(const WireTunnel& t) {
        tx_bytes_ += t.len;
        bool reliable = (t.flags & TF_RELIABLE) != 0;
        if (reliable) {
            // 乱序缓冲：按序投递
            if (t.seq == expected_seq_) {
                deliver(t.payload, t.len, t.channel_id);
                expected_seq_++;
                while (!ooo_.empty() && ooo_.begin()->first == expected_seq_) {
                    uint16_t seq = ooo_.begin()->first;
                    auto it = slot_channel.find(seq);
                    uint8_t ch = it != slot_channel.end() ? it->second : 0;
                    deliver(ooo_.begin()->second.data(), (uint16_t)ooo_.begin()->second.size(), ch);
                    slot_channel.erase(seq);
                    ooo_.erase(ooo_.begin());
                    expected_seq_++;
                }
            } else if (t.seq > expected_seq_ && ooo_.size() < 64) {
                // 复制负载：recv 缓冲会被复用，不能保存悬垂指针
                auto& slot = ooo_[t.seq];
                slot.assign(t.payload, t.payload + t.len);
                slot_channel[t.seq] = t.channel_id;
            }
            // 累计确认（已连续收到的最大 seq）
            send_ack();
        } else {
            deliver(t.payload, t.len, t.channel_id);
        }
    }

    void deliver(const uint8_t* payload, uint16_t len, uint8_t channel_id) {
        rx_bytes_ += len;
        if (on_data && len > 0) on_data(channel_id, payload, len);
    }

    void handle_ack(uint16_t ack) {
        while (!pending_.empty() &&
               (int16_t)(pending_.front().seq - ack) <= 0)
            pending_.pop_front();
    }

    void send_ack() {
        uint16_t ack = expected_seq_ > 0 ? (uint16_t)(expected_seq_ - 1) : 0;
        std::vector<uint8_t> frame(14);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_ACK, session_id_,
                           0, 0, 0, ack, nullptr, 0);
        if (on_tx) on_tx(frame.data(), frame.size());
    }

    void reply_pong() {
        std::vector<uint8_t> frame(14);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_PONG, session_id_,
                           0, 0, 0, 0, nullptr, 0);
        if (on_tx) on_tx(frame.data(), frame.size());
    }

    uint16_t  session_id_ = 0;
    bool      active_ = true;
    uint16_t  next_seq_ = 0;
    uint16_t  expected_seq_ = 0;
    uint32_t  rto_ms_ = 400;
    uint64_t  last_ping_ms_ = 0;
    std::deque<Msg> pending_;
    std::map<uint16_t, std::vector<uint8_t>> ooo_;
    std::map<uint16_t, uint8_t> slot_channel;
    uint64_t  tx_bytes_ = 0;
    uint64_t  rx_bytes_ = 0;
};

} // namespace p2p

#endif // P2P_SDK_SESSION_SESSION_H
