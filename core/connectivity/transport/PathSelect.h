#ifndef P2P_COMMON_PATH_SELECT_H
#define P2P_COMMON_PATH_SELECT_H

// 发送路径准入（对齐 EasyTier connectivity/transport + hole_punch/policy）。
// 只根据已观测事实选路，不碰 juice_* / socket。force_relay 跳过全部直连族。

#include <cstdint>

namespace p2p {

enum class PathKind : uint8_t {
    IceNominated = 0,  // 当前 ICE agent 已 nominated
    JuicePrev    = 1,  // ICE restart 期间旧 agent 继续扛媒体
    JuiceDirect  = 2,  // juice 已 direct_ok（尚未标 nominated）
    TcpPunch     = 3,  // EasyTier 式 TCP simultaneous-open
    UdpDirect    = 4,  // 自研 UDP 打洞 socket
    DerpTcp      = 5,  // Proxy TCP/TLS 中继
    UdpRelay     = 6,  // Proxy UDP 中继
    None         = 7,
};

struct PathFacts {
    bool force_relay     = false;
    bool ice_nominated   = false;
    bool juice_prev      = false;
    bool juice_direct    = false;
    bool tcp_punch_ok    = false;
    bool have_direct_udp = false;
    bool direct_proven   = false;
    bool relay_ready     = false;
    bool derp_ready      = false;
    bool udp_relay_ready = false;
};

inline const char* path_kind_str(PathKind k) {
    switch (k) {
    case PathKind::IceNominated: return "ice";
    case PathKind::JuicePrev:    return "juice_prev";
    case PathKind::JuiceDirect:  return "juice";
    case PathKind::TcpPunch:     return "tcp_punch";
    case PathKind::UdpDirect:    return "udp_direct";
    case PathKind::DerpTcp:      return "derp_tcp";
    case PathKind::UdpRelay:     return "udp_relay";
    case PathKind::None:         return "none";
    }
    return "none";
}

// 优先级：ICE 族 → TCP 打洞 → UDP 打洞 → DERP TCP → UDP 中继。
// ICE 三项互斥（先 nominated，再 prev，再 juice+direct_ok）。
// 打洞窗口内（!relay_ready）有公网口即允许 UdpDirect，避免空窗。
inline int fill_send_paths(const PathFacts& f, PathKind* out, int maxn) {
    int n = 0;
    auto add = [&](PathKind k) {
        if (n < maxn) out[n++] = k;
    };
    if (!f.force_relay) {
        if (f.ice_nominated) add(PathKind::IceNominated);
        else if (f.juice_prev) add(PathKind::JuicePrev);
        else if (f.juice_direct) add(PathKind::JuiceDirect);
        if (f.tcp_punch_ok) add(PathKind::TcpPunch);
        if (f.have_direct_udp && (f.direct_proven || !f.relay_ready))
            add(PathKind::UdpDirect);
    }
    if (f.relay_ready && f.derp_ready) add(PathKind::DerpTcp);
    if (f.relay_ready && f.udp_relay_ready) add(PathKind::UdpRelay);
    return n;
}

inline PathKind select_send_path(const PathFacts& f) {
    PathKind buf[8];
    const int n = fill_send_paths(f, buf, 8);
    return n > 0 ? buf[0] : PathKind::None;
}

}  // namespace p2p

#endif
