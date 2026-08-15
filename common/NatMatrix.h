#ifndef P2P_COMMON_NAT_MATRIX_H
#define P2P_COMMON_NAT_MATRIX_H

// RFC 4787 二维 NAT × 两端 → 打洞策略。
// 不改 force_relay；生日打洞默认关。EDM×EDM 必须中继。

#include "common/ProtoDef.h"

#include <cstdint>

namespace p2p {

enum class PunchStrategy : uint8_t {
    Ice      = 0,   // 标准 ICE（EIM×EIM / 锥型）
    Predict  = 1,   // NAT4E 端口预测（单调映射）
    Birthday = 2,   // 可选生日扫描（默认关）
    Relay    = 3,   // 必须中继
};

inline const char* nat_mapping_str(uint8_t m) {
    switch (m) {
    case NAT_MAP_EIM: return "EIM";
    case NAT_MAP_ADM: return "ADM";
    case NAT_MAP_EDM: return "EDM";
    default:          return "unknown";
    }
}

inline const char* nat_filter_str(uint8_t f) {
    switch (f) {
    case NAT_FLT_NONE: return "none";
    case NAT_FLT_ADDR: return "addr";
    case NAT_FLT_PORT: return "port";
    default:           return "unknown";
    }
}

inline const char* punch_strategy_str(PunchStrategy s) {
    switch (s) {
    case PunchStrategy::Ice:      return "ice";
    case PunchStrategy::Predict:  return "predict";
    case PunchStrategy::Birthday: return "birthday";
    case PunchStrategy::Relay:    return "relay";
    }
    return "unknown";
}

// 两端映射 → 策略。filter 不影响「能否映射」，只影响对端要从哪个五元组回包，
// 标准 ICE 已覆盖锥型过滤；此处只按 mapping + NAT4E step 决策。
inline PunchStrategy punch_strategy(uint8_t map_a, uint8_t map_b,
                                    int16_t step_a, int16_t step_b,
                                    bool birthday_enabled = false) {
    const bool edm_a = (map_a == NAT_MAP_EDM);
    const bool edm_b = (map_b == NAT_MAP_EDM);
    if (edm_a && edm_b)
        return birthday_enabled ? PunchStrategy::Birthday : PunchStrategy::Relay;
    if ((edm_a && step_a != 0 && !edm_b) || (edm_b && step_b != 0 && !edm_a))
        return PunchStrategy::Predict;
    if ((edm_a || edm_b) && birthday_enabled)
        return PunchStrategy::Birthday;
    return PunchStrategy::Ice;
}

} // namespace p2p

#endif // P2P_COMMON_NAT_MATRIX_H
