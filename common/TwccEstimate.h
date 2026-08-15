#ifndef P2P_TWCC_ESTIMATE_H
#define P2P_TWCC_ESTIMATE_H

// 精简 TWCC + 一维 Kalman（学 pion GCC / RFC 8888 思路，不引入完整 RTCP）
//   发送端给不可靠帧打 transport-wide seq（复用 TunnelFrame.seq）
//   接收端回 TT_TWCC：到达时间反馈
//   发送端用到达间隔差（recv_delta - send_delta）做 Kalman，判 overuse
//   输出建议码率，供 AbrController 混合

#include "common/AbrEstimate.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace p2p {

struct DelayKalman {
    double x = 0;       // 滤波后的到达间隔超额（ms），>0 排队增加
    double p = 100;     // 估计协方差
    double q = 0.25;    // 过程噪声
    double r = 4.0;     // 观测噪声

    double update(double z) {
        p += q;
        const double k = p / (p + r);
        x += k * (z - x);
        p *= (1.0 - k);
        return x;
    }
};

// 发送端：记录发出时间，消化反馈，更新 Kalman / 码率
struct TwccEstimator {
    DelayKalman kf;
    std::map<uint16_t, uint64_t> sent;  // seq -> send_ms
    uint64_t last_send_ms = 0;
    uint64_t last_recv_ms = 0;
    bool have_prev = false;
    int overuse = 0;                    // -1 under / 0 hold / +1 over
    int bw_kbps = ABR_BASE_KBPS;
    uint64_t fb_count = 0;

    void on_send(uint16_t seq, uint64_t send_ms) {
        sent[seq] = send_ms;
        while (sent.size() > 256) sent.erase(sent.begin());
    }

    void on_feedback(uint16_t seq, uint64_t recv_ms) {
        auto it = sent.find(seq);
        if (it == sent.end()) return;
        const uint64_t send_ms = it->second;
        sent.erase(it);
        fb_count++;
        if (have_prev && send_ms > last_send_ms && recv_ms >= last_recv_ms) {
            const double ds = (double)(send_ms - last_send_ms);
            const double dr = (double)(recv_ms - last_recv_ms);
            const double z = dr - ds;
            const double x = kf.update(z);
            if (x > 12.0) {
                overuse = 1;
                bw_kbps = bw_kbps * ABR_MD_NUM / ABR_MD_DEN;
            } else if (x < -6.0) {
                overuse = -1;
                bw_kbps += ABR_AI_KBPS;
            } else {
                overuse = 0;
            }
            if (bw_kbps < ABR_MIN_KBPS) bw_kbps = ABR_MIN_KBPS;
            if (bw_kbps > ABR_MAX_KBPS) bw_kbps = ABR_MAX_KBPS;
        }
        last_send_ms = send_ms;
        last_recv_ms = recv_ms;
        have_prev = true;
    }

    int suggested_kbps() const { return bw_kbps; }
};

// 接收端：攒到达样本，写出反馈负载
struct TwccReceiver {
    struct Item {
        uint16_t seq;
        uint32_t recv_ms;
    };
    std::vector<Item> pending;
    uint64_t first_ms = 0;

    void on_recv(uint16_t seq, uint64_t now) {
        if (pending.empty()) first_ms = now;
        pending.push_back({seq, (uint32_t)now});
        if (pending.size() > 16) pending.erase(pending.begin());
    }

    bool should_flush(uint64_t now) const {
        if (pending.empty()) return false;
        return pending.size() >= 4 || (now - first_ms) >= 50;
    }

    // 线格式：count(1) + N×(seq_be16 + recv_ms_be32)
    size_t write(uint8_t* out, size_t cap) {
        if (!out || pending.empty()) return 0;
        const uint8_t n = (uint8_t)pending.size();
        const size_t need = 1 + (size_t)n * 6;
        if (cap < need) return 0;
        out[0] = n;
        size_t o = 1;
        for (const auto& it : pending) {
            out[o++] = (uint8_t)(it.seq >> 8);
            out[o++] = (uint8_t)(it.seq & 0xFF);
            out[o++] = (uint8_t)(it.recv_ms >> 24);
            out[o++] = (uint8_t)(it.recv_ms >> 16);
            out[o++] = (uint8_t)(it.recv_ms >> 8);
            out[o++] = (uint8_t)(it.recv_ms & 0xFF);
        }
        pending.clear();
        return o;
    }

    static void parse(const uint8_t* p, size_t n, TwccEstimator& est) {
        if (!p || n < 1) return;
        const uint8_t cnt = p[0];
        if (n < 1 + (size_t)cnt * 6) return;
        size_t o = 1;
        for (uint8_t i = 0; i < cnt; i++) {
            const uint16_t seq = (uint16_t)((p[o] << 8) | p[o + 1]);
            o += 2;
            const uint32_t recv = ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) |
                                  ((uint32_t)p[o + 2] << 8) | (uint32_t)p[o + 3];
            o += 4;
            est.on_feedback(seq, recv);
        }
    }
};

} // namespace p2p

#endif // P2P_TWCC_ESTIMATE_H
