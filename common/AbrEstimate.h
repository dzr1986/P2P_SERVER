#ifndef P2P_ABR_ESTIMATE_H
#define P2P_ABR_ESTIMATE_H

// delay-based 目标档 + RTT 趋势 AIMD（学 pion GCC / WebRTC overuse detector）
//   abr_suggest_kbps：纯函数，SRTT/RTTVAR/丢包/cwnd → 目标档
//   AbrController：有状态，向目标做加性增 / 乘性减，避免码率台阶跳变
// AV 层 avSuggestedBitrateKbps 走 AbrController。

#include <cstdint>

namespace p2p {

constexpr int ABR_MIN_KBPS = 200;
constexpr int ABR_MAX_KBPS = 8000;
constexpr int ABR_BASE_KBPS = 4000;
constexpr int ABR_AI_KBPS = 200;     // 加性增：每步 +200 kbps
constexpr int ABR_MD_NUM = 7;        // 乘性减：×7/8
constexpr int ABR_MD_DEN = 8;
constexpr uint32_t ABR_RTT_TREND_MS = 8;  // SRTT 变化超过此值视为趋势

inline int abr_suggest_kbps(uint32_t srtt_ms, uint32_t rttvar_ms,
                            uint32_t cwnd, uint64_t rx_lost) {
    int kbps = ABR_BASE_KBPS;
    // delay-based overuse / underuse（对标 GCC 的 overuse detector 粗分档）
    if (srtt_ms >= 400 || rttvar_ms >= 80) {
        kbps = 800;          // 明显过载：保 480p
    } else if (srtt_ms >= 200 || rttvar_ms >= 40) {
        kbps = 2000;         // 轻度过载：720p 偏低
    } else if (srtt_ms > 0 && srtt_ms < 60 && rttvar_ms < 15) {
        kbps = 6000;         // 空闲：允许 1080p
    }

    if (cwnd > 0 && cwnd < 8) {
        kbps = kbps * (int)cwnd / 8;
    }
    if (rx_lost > 0) {
        kbps = kbps * 3 / 4;
    }
    if (kbps < ABR_MIN_KBPS) kbps = ABR_MIN_KBPS;
    if (kbps > ABR_MAX_KBPS) kbps = ABR_MAX_KBPS;
    return kbps;
}

// 一步 AIMD：last 向 target 靠拢；rising 时强制乘性减。
inline int abr_aimd_step(int last_kbps, int target_kbps, bool rtt_rising) {
    if (last_kbps <= 0) return target_kbps;
    int next = last_kbps;
    if (rtt_rising || target_kbps < last_kbps) {
        next = last_kbps * ABR_MD_NUM / ABR_MD_DEN;
        if (target_kbps < next) next = (next + target_kbps) / 2;
    } else if (target_kbps > last_kbps) {
        next = last_kbps + ABR_AI_KBPS;
        if (next > target_kbps) next = target_kbps;
    }
    if (next < ABR_MIN_KBPS) next = ABR_MIN_KBPS;
    if (next > ABR_MAX_KBPS) next = ABR_MAX_KBPS;
    return next;
}

struct AbrController {
    int last_kbps = 0;
    uint32_t last_srtt_ms = 0;

    int update(uint32_t srtt_ms, uint32_t rttvar_ms,
               uint32_t cwnd, uint64_t rx_lost) {
        const int target = abr_suggest_kbps(srtt_ms, rttvar_ms, cwnd, rx_lost);
        const bool rising = last_srtt_ms > 0 &&
                            srtt_ms > last_srtt_ms + ABR_RTT_TREND_MS;
        last_kbps = abr_aimd_step(last_kbps, target, rising);
        last_srtt_ms = srtt_ms;
        return last_kbps;
    }
};

} // namespace p2p

#endif // P2P_ABR_ESTIMATE_H
