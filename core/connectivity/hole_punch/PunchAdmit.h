#ifndef P2P_CORE_HOLE_PUNCH_PUNCH_ADMIT_H
#define P2P_CORE_HOLE_PUNCH_PUNCH_ADMIT_H

// CONNECT 打洞准入（学 EasyTier hole_punch/policy + NatMatrix）。
// 只给「建议」：Relay 时客户端仍打洞，但更快开中继。不改 force_relay。
// 生日扫描只在客户端开关，服务端按默认关计算（EDM×EDM → Relay）。

#include "core/connectivity/hole_punch/NatMatrix.h"
#include "core/packet/ProtoDef.h"

#include <cstdint>

namespace p2p {

enum class PunchAdmit : uint8_t {
    Ice     = 1,
    Relay   = 2,
    Unknown = 0,
};

// 挂在 CONNECT_ACK / INVITE 定长结构之后的 1 字节；旧客户端忽略尾部。
constexpr uint8_t PUNCH_HINT_NONE  = 0;
constexpr uint8_t PUNCH_HINT_ICE   = 1;
constexpr uint8_t PUNCH_HINT_RELAY = 2;

// Relay 提示时仍打洞，但中继等待从 connect_timeout 收到这个值。
constexpr uint32_t PUNCH_HINT_RELAY_WAIT_MS = 1500;

inline const char* punch_admit_str(PunchAdmit a) {
    switch (a) {
    case PunchAdmit::Ice:     return "ice";
    case PunchAdmit::Relay:   return "relay";
    case PunchAdmit::Unknown: return "unknown";
    }
    return "unknown";
}

inline uint8_t punch_admit_hint(PunchAdmit a) {
    switch (a) {
    case PunchAdmit::Ice:   return PUNCH_HINT_ICE;
    case PunchAdmit::Relay: return PUNCH_HINT_RELAY;
    default:                return PUNCH_HINT_NONE;
    }
}

inline PunchAdmit admit_connect_punch(uint8_t src_nat, uint8_t dst_nat) {
    const uint8_t a = four_type_to_mapping(src_nat);
    const uint8_t b = four_type_to_mapping(dst_nat);
    if (a == NAT_MAP_UNKNOWN || b == NAT_MAP_UNKNOWN) return PunchAdmit::Unknown;
    const PunchStrategy st = punch_strategy(a, b, 0, 0, false);
    return st == PunchStrategy::Relay ? PunchAdmit::Relay : PunchAdmit::Ice;
}

inline uint32_t punch_wait_ms(uint8_t hint, uint32_t default_ms) {
    if (hint == PUNCH_HINT_RELAY) return PUNCH_HINT_RELAY_WAIT_MS;
    return default_ms;
}

}  // namespace p2p

#endif
