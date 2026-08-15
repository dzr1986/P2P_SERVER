#ifndef P2P_ABR_ESTIMATE_H
#define P2P_ABR_ESTIMATE_H

// 简化 delay-based 码率建议（学 pion GCC / WebRTC overuse detector 的输入信号）
// 不实现完整 Kalman + AIMD 控制器，只用 SRTT / RTTVAR / 丢包 / cwnd 做分段决策：
//   - 高延迟或高抖动 → 判定 overuse，压到低档
//   - 低延迟且低抖动 → underuse，允许更高档（1080p 预览）
//   - 丢包 / 拥塞窗口收缩 → 再打折
// 纯函数，便于单测；AV 层 avSuggestedBitrateKbps 直接调用。

#include <cstdint>

namespace p2p {

constexpr int ABR_MIN_KBPS = 200;
constexpr int ABR_MAX_KBPS = 8000;
constexpr int ABR_BASE_KBPS = 4000;

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

} // namespace p2p

#endif // P2P_ABR_ESTIMATE_H
