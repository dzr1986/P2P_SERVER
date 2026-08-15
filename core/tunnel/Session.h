#ifndef P2P_SDK_SESSION_SESSION_H
#define P2P_SDK_SESSION_SESSION_H

// 对端隧道会话：逻辑通道 + 可靠 UDP（seq/ack/重传/乱序缓冲）
// 与传输解耦：会话不感知打洞直连或中继，产出完整 TunnelFrame 字节流交由传输层投递
//
// 流媒体级传输特性（第二轮优化，参考 KCP / SRT / TCP RFC 6298/5681）：
//   - 自适应 RTO：SRTT/RTTVAR 平滑估计（RFC 6298），Karn 算法排除重传帧采样，
//     超时退避 1.5x（KCP 风格，比 TCP 2x 更利于低延迟收敛）
//   - 快速重传：3 次重复累计 ACK 立即重传首个未确认帧，不等 RTO
//   - 拥塞窗口：慢启动 + 拥塞避免（RFC 5681 简化版，对齐 KCP cwnd/ssthresh）
//   - 不可靠通道 FEC：每 kFecGroup 帧发一个 XOR 校验帧（TT_FEC），
//     单帧丢失免重传恢复（参考 SRT/flexfec 思路，kcp-go 用 Reed-Solomon 为进阶方向）
//   - LinkStats：srtt/丢包/重传/FEC 统计，供上层自适应码率（对齐 WebRTC GCC 的输入）

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "core/packet/ProtoDef.h"
#include "core/tunnel/TwccEstimate.h"
#include "core/foundation/Plat.h"
#include "core/packet/Codec.h"

namespace p2p {

class Session {
public:
    struct Msg {
        uint16_t  seq;
        uint8_t   channel_id;
        uint8_t   flags;
        std::vector<uint8_t> data;   // 完整 TunnelFrame + 负载（直接发送）
        uint64_t  sent_ms;
        bool      retransmitted = false;   // Karn：重传过的帧不参与 RTT 采样
    };

    // 链路统计：供上层做自适应码率 / 路径质量判定
    struct LinkStats {
        uint32_t srtt_ms = 0;        // 平滑 RTT
        uint32_t rttvar_ms = 0;      // RTT 抖动
        uint32_t rto_ms = 0;         // 当前重传超时
        uint32_t cwnd = 0;           // 拥塞窗口（帧）
        uint32_t inflight = 0;       // 未确认帧数
        uint64_t tx_bytes = 0;
        uint64_t rx_bytes = 0;
        uint64_t retrans = 0;        // 超时重传次数
        uint64_t fast_retrans = 0;   // 快速重传次数
        uint64_t fec_sent = 0;       // 发出的 FEC 校验帧
        uint64_t fec_recovered = 0;  // FEC 恢复的丢帧
        uint64_t rx_lost = 0;        // 不可靠通道丢包估计（seq 空洞，恢复后回补）
        uint32_t twcc_kbps = 0;      // TWCC+Kalman 建议码率（0=尚未采样）
        int32_t  twcc_overuse = 0;   // -1 under / 0 hold / +1 over
    };

    // 交付回调：业务数据（channel, payload, len）
    std::function<void(uint8_t, const uint8_t*, size_t)> on_data;
    // 发送回调：完整隧道帧字节，交由传输层（直连/中继）
    std::function<void(const uint8_t*, size_t)> on_tx;

    explicit Session(uint16_t session_id) : session_id_(session_id) {}

    uint16_t id() const { return session_id_; }
    bool active() const { return active_; }
    void deactivate() { active_ = false; }
    void set_fec_enabled(bool on) { fec_enabled_ = on; }

    // ---- 发送 ----
    // 返回 0 成功，-1 参数错误，-2 可靠窗口已满（拥塞控制限流，上层应降码率/缓发）
    int send(uint8_t channel_id, const void* data, size_t len, bool reliable) {
        if (len > MAX_TUNNEL_PAYLOAD) return -1;
        const uint8_t* p = (const uint8_t*)data;
        uint16_t seq;
        uint8_t  flags = 0;
        if (reliable) {
            flags |= TF_RELIABLE;
            uint32_t wnd = cwnd_ < kMaxWnd ? cwnd_ : kMaxWnd;
            if (pending_.size() >= wnd) return -2;   // 拥塞窗口满
            seq = next_seq_++;
        } else {
            seq = unrel_seq_++;
        }
        std::vector<uint8_t> frame(14 + len);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_DATA, session_id_,
                           channel_id, flags, seq, 0, p, (uint16_t)len);
        tx_bytes_ += len;
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
        // FEC 累积必须在数据帧发出之后：满组 flush 的校验帧不能先于组尾帧到达
        if (!reliable && fec_enabled_) fec_accumulate(channel_id, p, (uint16_t)len, seq);
        if (!reliable) twcc_tx_.on_send(seq, plat_now_ms());
        return 0;
    }

    // ---- 接收（输入完整隧道帧字节） ----
    void on_frame(const uint8_t* buf, size_t len) {
        WireTunnel t;
        if (!codec_read_tunnel(buf, len, t)) return;
        switch (t.type) {
        case TT_DATA:   handle_data(t);  break;
        case TT_ACK:    handle_ack(t.ack); break;
        case TT_FEC:    handle_fec(t); break;
        case TT_TWCC:   TwccReceiver::parse(t.payload, t.len, twcc_tx_); break;
        case TT_PING:   reply_pong(); break;
        case TT_CLOSE:  active_ = false; break;
        case TT_PONG:   break;  // 保活确认，无需处理
        default:        break;
        }
    }

    // 周期驱动：重传超时未确认帧 + FEC 部分组冲刷 + 通道保活
    void tick(uint64_t now) {
        if (now - last_ping_ms_ >= 15000) {   // 隧道级保活
            last_ping_ms_ = now;
            std::vector<uint8_t> frame(14);
            codec_write_tunnel(frame.data(), (int)frame.size(), TT_PING, session_id_,
                               0, 0, 0, 0, nullptr, 0);
            if (on_tx) on_tx(frame.data(), frame.size());
        }
        // 部分 FEC 组超时冲刷：低速流不必凑满一组才获得保护
        if (fec_count_ > 0 && now - fec_first_ms_ >= 100) fec_flush();
        flush_twcc(now);

        bool timeout_loss = false;
        for (auto& m : pending_) {
            if (now - m.sent_ms >= rto_ms_) {
                if (on_tx) on_tx(m.data.data(), m.data.size());
                m.sent_ms = now;
                m.retransmitted = true;
                retrans_++;
                timeout_loss = true;
            }
        }
        if (timeout_loss) {
            // RFC 5681：超时视为拥塞，窗口回退；RTO 1.5x 退避（KCP 风格）
            uint32_t inflight = (uint32_t)pending_.size();
            ssthresh_ = inflight / 2 > 2 ? inflight / 2 : 2;
            cwnd_ = 2;
            ca_acc_ = 0;
            rto_ms_ = rto_ms_ * 3 / 2;
            if (rto_ms_ > kMaxRto) rto_ms_ = kMaxRto;
        }
    }

    // 统计
    size_t pending_size() const { return pending_.size(); }
    uint64_t tx_bytes() const { return tx_bytes_; }
    uint64_t rx_bytes() const { return rx_bytes_; }
    LinkStats stats() const {
        LinkStats s;
        s.srtt_ms = srtt_ms_;
        s.rttvar_ms = rttvar_ms_;
        s.rto_ms = rto_ms_;
        s.cwnd = cwnd_;
        s.inflight = (uint32_t)pending_.size();
        s.tx_bytes = tx_bytes_;
        s.rx_bytes = rx_bytes_;
        s.retrans = retrans_;
        s.fast_retrans = fast_retrans_;
        s.fec_sent = fec_sent_;
        s.fec_recovered = fec_recovered_;
        s.rx_lost = rx_lost_;
        s.twcc_kbps = (uint32_t)twcc_tx_.suggested_kbps();
        s.twcc_overuse = twcc_tx_.overuse;
        return s;
    }

private:
    static constexpr uint32_t kMaxWnd = 64;      // 硬窗口上限（对齐原实现）
    static constexpr uint32_t kInitCwnd = 4;
    static constexpr uint32_t kInitSsthresh = 32;
    static constexpr uint32_t kMinRto = 100;     // 流媒体场景下限（tick 粒度 50ms）
    static constexpr uint32_t kMaxRto = 3000;
    static constexpr int      kFecGroup = 4;     // 每 4 个不可靠帧出 1 个 XOR 校验帧
    static constexpr size_t   kUnrelCacheMax = 128;

    void handle_data(const WireTunnel& t) {
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
            // 不可靠通道：即时投递（流媒体低延迟优先），FEC 缓存 + 丢包估计
            // track 返回 true 表示该 seq 已投递过（如 FEC 先行恢复后原帧迟到），去重
            twcc_rx_.on_recv(t.seq, plat_now_ms());
            if (!track_unreliable(t)) deliver(t.payload, t.len, t.channel_id);
        }
    }

    void flush_twcc(uint64_t now) {
        if (!twcc_rx_.should_flush(now)) return;
        uint8_t fb[1 + 16 * 6];
        const size_t n = twcc_rx_.write(fb, sizeof(fb));
        if (n == 0 || !on_tx) return;
        std::vector<uint8_t> frame(14 + n);
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_TWCC, session_id_,
                           0, 0, 0, 0, fb, (uint16_t)n);
        on_tx(frame.data(), frame.size());
    }

    void deliver(const uint8_t* payload, uint16_t len, uint8_t channel_id) {
        rx_bytes_ += len;
        if (on_data && len > 0) on_data(channel_id, payload, len);
    }

    void handle_ack(uint16_t ack) {
        uint64_t now = plat_now_ms();
        uint32_t acked = 0;
        // Karn 算法：仅未重传帧参与采样；累计 ACK 会连带确认因队首丢包被阻塞的旧帧，
        // 其等待时间含重传恢复延迟，故取批次内最小样本（最接近真实链路 RTT）
        uint32_t best_sample = 0;
        while (!pending_.empty() &&
               (int16_t)(pending_.front().seq - ack) <= 0) {
            Msg& m = pending_.front();
            if (!m.retransmitted) {
                uint32_t r = (uint32_t)(now - m.sent_ms);
                if (r == 0) r = 1;
                if (best_sample == 0 || r < best_sample) best_sample = r;
            }
            pending_.pop_front();
            acked++;
        }
        if (best_sample > 0) update_rtt(best_sample);
        if (acked > 0) {
            dup_acks_ = 0;
            last_ack_ = ack;
            grow_cwnd(acked);
            return;
        }
        // 重复累计 ACK：3 次触发快速重传（RFC 5681 / KCP fast resend）
        if (!pending_.empty() && ack == last_ack_ && ++dup_acks_ >= 3) {
            dup_acks_ = 0;
            Msg& m = pending_.front();
            if (on_tx) on_tx(m.data.data(), m.data.size());
            m.sent_ms = now;
            m.retransmitted = true;
            fast_retrans_++;
            uint32_t half = cwnd_ / 2;
            ssthresh_ = half > 2 ? half : 2;
            cwnd_ = ssthresh_;   // 快速恢复：不回到 1
            ca_acc_ = 0;
        }
    }

    // RFC 6298：SRTT/RTTVAR 平滑与 RTO 计算
    void update_rtt(uint32_t r_ms) {
        if (srtt_ms_ == 0) {
            srtt_ms_ = r_ms;
            rttvar_ms_ = r_ms / 2;
        } else {
            uint32_t delta = srtt_ms_ > r_ms ? srtt_ms_ - r_ms : r_ms - srtt_ms_;
            rttvar_ms_ = (3 * rttvar_ms_ + delta) / 4;
            srtt_ms_ = (7 * srtt_ms_ + r_ms) / 8;
        }
        uint32_t var4 = 4 * rttvar_ms_ > 50 ? 4 * rttvar_ms_ : 50;
        rto_ms_ = srtt_ms_ + var4;
        if (rto_ms_ < kMinRto) rto_ms_ = kMinRto;
        if (rto_ms_ > kMaxRto) rto_ms_ = kMaxRto;
    }

    // RFC 5681：慢启动指数增窗，拥塞避免每 RTT 线性 +1
    void grow_cwnd(uint32_t acked) {
        if (cwnd_ < ssthresh_) {
            cwnd_ += acked;
            if (cwnd_ > ssthresh_) cwnd_ = ssthresh_;
        } else {
            ca_acc_ += acked;
            if (ca_acc_ >= cwnd_) {
                ca_acc_ = 0;
                cwnd_++;
            }
        }
        if (cwnd_ > kMaxWnd) cwnd_ = kMaxWnd;
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

    // ---- 不可靠通道 FEC（发送侧：XOR 累积一组帧） ----
    // 校验块布局：xor over [len(2B) | channel(1B) | payload(补零至组内最大)]
    void fec_accumulate(uint8_t channel, const uint8_t* p, uint16_t len, uint16_t seq) {
        if (fec_count_ == 0) {
            memset(fec_xor_, 0, sizeof(fec_xor_));
            fec_base_seq_ = seq;
            fec_max_len_ = 0;
            fec_first_ms_ = plat_now_ms();
        }
        fec_xor_[0] ^= (uint8_t)(len >> 8);
        fec_xor_[1] ^= (uint8_t)(len & 0xFF);
        fec_xor_[2] ^= channel;
        for (uint16_t i = 0; i < len; i++) fec_xor_[3 + i] ^= p[i];
        if (len > fec_max_len_) fec_max_len_ = len;
        fec_count_++;
        if (fec_count_ >= kFecGroup) fec_flush();
    }

    void fec_flush() {
        if (fec_count_ == 0) return;
        uint16_t plen = (uint16_t)(3 + fec_max_len_);
        std::vector<uint8_t> frame(14 + plen);
        // seq=组基序号，ack=组内帧数（复用现有帧头字段，不改帧格式）
        codec_write_tunnel(frame.data(), (int)frame.size(), TT_FEC, session_id_,
                           0, 0, fec_base_seq_, fec_count_, fec_xor_, plen);
        if (on_tx) on_tx(frame.data(), frame.size());
        fec_sent_++;
        fec_count_ = 0;
    }

    // ---- 不可靠通道 FEC（接收侧：缓存 + 单帧恢复） ----
    // 返回 true 表示该 seq 已在缓存中（已直达或已被 FEC 恢复），调用方应跳过投递
    bool track_unreliable(const WireTunnel& t) {
        // 丢包估计：seq 空洞（乱序会短暂高估，FEC 恢复后回补）
        if (unrel_inited_) {
            int16_t gap = (int16_t)(t.seq - expected_unrel_);
            if (gap > 0) rx_lost_ += gap;
            if (gap >= 0) expected_unrel_ = (uint16_t)(t.seq + 1);
        } else {
            unrel_inited_ = true;
            expected_unrel_ = (uint16_t)(t.seq + 1);
        }
        if (!fec_enabled_) return false;
        if (unrel_cache_.find(t.seq) != unrel_cache_.end()) return true;   // 迟到重复帧
        // FIFO 缓存最近的不可靠帧供 FEC 恢复
        if (unrel_order_.size() >= kUnrelCacheMax) {
            unrel_cache_.erase(unrel_order_.front());
            unrel_order_.pop_front();
        }
        auto& slot = unrel_cache_[t.seq];
        slot.channel = t.channel_id;
        slot.payload.assign(t.payload, t.payload + t.len);
        unrel_order_.push_back(t.seq);
        return false;
    }

    void handle_fec(const WireTunnel& t) {
        if (!fec_enabled_ || t.len < 3) return;
        uint16_t base = t.seq;
        uint16_t count = t.ack;
        if (count == 0 || count > kFecGroup) return;

        // 统计组内缺失
        int missing = 0;
        uint16_t missing_seq = 0;
        for (uint16_t i = 0; i < count; i++) {
            uint16_t s = (uint16_t)(base + i);
            if (unrel_cache_.find(s) == unrel_cache_.end()) {
                missing++;
                missing_seq = s;
            }
        }
        if (missing != 1) return;   // 0 缺失无需恢复；>1 XOR 无法恢复

        // XOR 已收帧与校验块，还原缺失帧的 len/channel/payload
        std::vector<uint8_t> rec(t.payload, t.payload + t.len);
        for (uint16_t i = 0; i < count; i++) {
            uint16_t s = (uint16_t)(base + i);
            auto it = unrel_cache_.find(s);
            if (it == unrel_cache_.end()) continue;
            const auto& e = it->second;
            uint16_t elen = (uint16_t)e.payload.size();
            rec[0] ^= (uint8_t)(elen >> 8);
            rec[1] ^= (uint8_t)(elen & 0xFF);
            rec[2] ^= e.channel;
            size_t n = e.payload.size() < rec.size() - 3 ? e.payload.size() : rec.size() - 3;
            for (size_t j = 0; j < n; j++) rec[3 + j] ^= e.payload[j];
        }
        uint16_t rlen = (uint16_t)((rec[0] << 8) | rec[1]);
        uint8_t rch = rec[2];
        if (rlen == 0 || (size_t)rlen > rec.size() - 3) return;   // 校验不一致，放弃

        fec_recovered_++;
        if (rx_lost_ > 0) rx_lost_--;
        // 缓存恢复帧，避免同组重复恢复
        if (unrel_order_.size() >= kUnrelCacheMax) {
            unrel_cache_.erase(unrel_order_.front());
            unrel_order_.pop_front();
        }
        auto& slot = unrel_cache_[missing_seq];
        slot.channel = rch;
        slot.payload.assign(rec.data() + 3, rec.data() + 3 + rlen);
        unrel_order_.push_back(missing_seq);
        deliver(rec.data() + 3, rlen, rch);
    }

    struct UnrelEntry {
        uint8_t channel = 0;
        std::vector<uint8_t> payload;
    };

    uint16_t  session_id_ = 0;
    bool      active_ = true;
    uint16_t  next_seq_ = 0;
    uint16_t  expected_seq_ = 0;
    uint64_t  last_ping_ms_ = 0;
    std::deque<Msg> pending_;
    std::map<uint16_t, std::vector<uint8_t>> ooo_;
    std::map<uint16_t, uint8_t> slot_channel;
    uint64_t  tx_bytes_ = 0;
    uint64_t  rx_bytes_ = 0;

    // 自适应 RTO / 拥塞控制
    uint32_t  srtt_ms_ = 0;
    uint32_t  rttvar_ms_ = 0;
    uint32_t  rto_ms_ = 300;
    uint32_t  cwnd_ = kInitCwnd;
    uint32_t  ssthresh_ = kInitSsthresh;
    uint32_t  ca_acc_ = 0;
    uint16_t  last_ack_ = 0xFFFF;
    uint32_t  dup_acks_ = 0;
    uint64_t  retrans_ = 0;
    uint64_t  fast_retrans_ = 0;

    // 不可靠通道 seq / FEC
    bool      fec_enabled_ = true;
    uint16_t  unrel_seq_ = 0;
    bool      unrel_inited_ = false;
    uint16_t  expected_unrel_ = 0;
    uint64_t  rx_lost_ = 0;
    uint8_t   fec_xor_[3 + MAX_TUNNEL_PAYLOAD] = {0};
    uint16_t  fec_max_len_ = 0;
    uint16_t  fec_base_seq_ = 0;
    int       fec_count_ = 0;
    uint64_t  fec_first_ms_ = 0;
    uint64_t  fec_sent_ = 0;
    uint64_t  fec_recovered_ = 0;
    std::unordered_map<uint16_t, UnrelEntry> unrel_cache_;
    std::deque<uint16_t> unrel_order_;

    TwccEstimator twcc_tx_;
    TwccReceiver  twcc_rx_;
};

} // namespace p2p

#endif // P2P_SDK_SESSION_SESSION_H
