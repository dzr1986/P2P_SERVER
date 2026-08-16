#ifndef P2P_CORE_HOLE_PUNCH_PUNCH_ADMIT_H
#define P2P_CORE_HOLE_PUNCH_PUNCH_ADMIT_H

// CONNECT 打洞准入（学 guide/aboutp2p.md 难度表 + hole_punch/policy）。
// 锥×锥 / 锥×对称 → Ice；对称×对称（极难）→ Relay。
// 只给「建议」：Relay 时客户端仍打洞，但更快开中继。不改 force_relay。

#include "core/connectivity/hole_punch/NatMatrix.h"
#include "core/packet/ProtoDef.h"

#include <cstdint>
#include <string>

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
constexpr uint8_t PUNCH_HINT_NEED  = 3;  // 任一侧 need_p2p：满超时打洞（EDM×EDM 仍 Relay）

// 心跳 extinfo 里宣告 need_p2p；旧服务端当不透明串。
constexpr const char* NEED_P2P_EXT = "np=1";

// Relay 提示时仍打洞，但中继等待从 connect_timeout 收到这个值。
constexpr uint32_t PUNCH_HINT_RELAY_WAIT_MS = 1500;

inline bool extinfo_has_need_p2p(const std::string& ext) {
    return ext.find(NEED_P2P_EXT) != std::string::npos;
}

inline const char* punch_admit_str(PunchAdmit a) {
    switch (a) {
    case PunchAdmit::Ice:     return "ice";
    case PunchAdmit::Relay:   return "relay";
    case PunchAdmit::Unknown: return "unknown";
    }
    return "unknown";
}

inline const char* punch_hint_str(uint8_t hint) {
    switch (hint) {
    case PUNCH_HINT_ICE:   return "ice";
    case PUNCH_HINT_RELAY: return "relay";
    case PUNCH_HINT_NEED:  return "need";
    default:               return "none";
    }
}

inline uint8_t punch_admit_hint(PunchAdmit a) {
    switch (a) {
    case PunchAdmit::Ice:   return PUNCH_HINT_ICE;
    case PunchAdmit::Relay: return PUNCH_HINT_RELAY;
    default:                return PUNCH_HINT_NONE;
    }
}

// EDM×EDM 仍 Relay；其余任一侧 need_p2p 则 NEED。不改 force_relay。
inline uint8_t compose_connect_hint(PunchAdmit admit, bool src_need, bool dst_need) {
    const uint8_t hint = punch_admit_hint(admit);
    if (hint != PUNCH_HINT_RELAY && (src_need || dst_need)) return PUNCH_HINT_NEED;
    return hint;
}

// 心跳尚未带上自检结果时，用服务端观察值补全。
inline uint8_t resolve_nattype(uint8_t reported, uint8_t observed) {
    return reported != NAT_UNKNOWN ? reported : observed;
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
    return default_ms;  // Ice / Need / 旧服务端：满超时
}

}  // namespace p2p

#endif
