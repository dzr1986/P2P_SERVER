#ifndef P2P_COMMON_NAT_MATRIX_H
#define P2P_COMMON_NAT_MATRIX_H

// RFC 4787 二维 NAT × 两端 → 打洞策略。
// 不改 force_relay；生日打洞默认关。EDM×EDM 必须中继。

#include "core/packet/ProtoDef.h"

#include <cstdint>
#include <cstring>

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
inline uint8_t four_type_to_mapping(uint8_t nattype) {
    switch (nattype) {
    case NAT_FULL_CONE:
    case NAT_PORT_RESTRICTED: return NAT_MAP_EIM;
    case NAT_SYMMETRIC:       return NAT_MAP_EDM;
    default:                  return NAT_MAP_UNKNOWN;
    }
}

// juice / "ip:port" / "[v6]:port" → 是否 IPv6（NAT66 也算 v6 路径，不假设必通）
inline bool sdp_has_ipv6_host(const char* sdp) {
    if (!sdp) return false;
    for (const char* p = sdp; *p; ) {
        const char* line = p;
        while (*p && *p != '\n') ++p;
        if (p - line > 12 && strncmp(line, "a=candidate", 11) == 0) {
            bool colon = false, dot = false, brack = false;
            for (const char* q = line; q < p; ++q) {
                if (*q == '[') brack = true;
                else if (*q == '.') dot = true;
                else if (*q == ':') colon = true;
            }
            if (brack || (colon && !dot)) return true;
        }
        if (*p == '\n') ++p;
    }
    return false;
}

inline bool path_addr_is_ipv6(const char* s) {
    if (!s || !s[0]) return false;
    if (s[0] == '[') return true;
    for (const char* p = s; *p && *p != ':'; ++p) {
        if (*p == '.') return false;
    }
    // 无点且含冒号：v6 或 ":port" 残缺；两个及以上冒号才是 v6
    int colons = 0;
    for (const char* p = s; *p; ++p) if (*p == ':') colons++;
    return colons >= 2;
}

// 生日/预测目的端口：base, ±step, ±2step… n 上限 256。step=0 则用 1。
inline size_t birthday_dest_ports(uint16_t base, int16_t step, uint16_t n,
                                  uint16_t* out, size_t cap) {
    if (!out || cap == 0 || n == 0 || base == 0) return 0;
    if (n > 256) n = 256;
    if (step == 0) step = 1;
    size_t w = 0;
    auto push = [&](int p) {
        if (p > 0 && p <= 65535 && w < n && w < cap) out[w++] = (uint16_t)p;
    };
    push((int)base);
    for (int k = 1; w < n && w < cap; k++) {
        push((int)base + (int)step * k);
        push((int)base - (int)step * k);
    }
    return w;
}

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
