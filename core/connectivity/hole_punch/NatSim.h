#ifndef P2P_COMMON_NAT_SIM_H
#define P2P_COMMON_NAT_SIM_H

// 用户态 RFC 4787 NAT 盒：CI 无 iptables/netns 时对照 punch_strategy()。
// 只模拟「先 STUN 再向对端 STUN 映射口打洞」；不替代现网 docker 矩阵。

#include "core/connectivity/hole_punch/NatMatrix.h"
#include "core/packet/ProtoDef.h"

#include <cstdint>
#include <vector>

namespace p2p {

struct NatSimBox {
    uint8_t mapping = NAT_MAP_EIM;
    uint8_t filter  = NAT_FLT_NONE;
    int16_t step    = 1;
    uint16_t next_ext = 50000;

    struct Map {
        uint32_t lan_ip = 0;
        uint16_t lan_port = 0;
        uint32_t dest_ip = 0;
        uint16_t dest_port = 0;
        uint16_t ext_port = 0;
        uint32_t opened_ip = 0;
        uint16_t opened_port = 0;
    };
    std::vector<Map> maps;

    uint16_t outbound(uint32_t lan_ip, uint16_t lan_port,
                      uint32_t dest_ip, uint16_t dest_port) {
        if (mapping == NAT_MAP_EIM) {
            for (auto& m : maps) {
                if (m.lan_ip == lan_ip && m.lan_port == lan_port) {
                    m.opened_ip = dest_ip;
                    m.opened_port = dest_port;
                    return m.ext_port;
                }
            }
        } else {
            for (auto& m : maps) {
                if (m.lan_ip == lan_ip && m.lan_port == lan_port &&
                    m.dest_ip == dest_ip && m.dest_port == dest_port) {
                    m.opened_ip = dest_ip;
                    m.opened_port = dest_port;
                    return m.ext_port;
                }
            }
        }
        Map m;
        m.lan_ip = lan_ip;
        m.lan_port = lan_port;
        m.dest_ip = dest_ip;
        m.dest_port = dest_port;
        m.ext_port = next_ext;
        m.opened_ip = dest_ip;
        m.opened_port = dest_port;
        if (step == 0) step = 1;
        next_ext = (uint16_t)(next_ext + (uint16_t)(step > 0 ? step : -step));
        maps.push_back(m);
        return m.ext_port;
    }

    bool inbound(uint32_t from_ip, uint16_t from_port, uint16_t ext_port) const {
        const Map* hit = nullptr;
        for (const auto& m : maps) {
            if (m.ext_port == ext_port) { hit = &m; break; }
        }
        if (!hit) return false;
        if (mapping == NAT_MAP_EDM &&
            (hit->dest_ip != from_ip || hit->dest_port != from_port))
            return false;
        if (filter == NAT_FLT_NONE) return true;
        if (filter == NAT_FLT_ADDR) return from_ip == hit->opened_ip;
        if (filter == NAT_FLT_PORT)
            return from_ip == hit->opened_ip && from_port == hit->opened_port;
        return false;
    }
};

// 两端先向 STUN 取映射，再互打对方的 STUN 口（标准 ICE，无预测）。
inline bool nat_sim_stun_punch(uint8_t map_a, uint8_t flt_a,
                               uint8_t map_b, uint8_t flt_b) {
    NatSimBox a, b;
    a.mapping = map_a; a.filter = flt_a; a.next_ext = 40000;
    b.mapping = map_b; b.filter = flt_b; b.next_ext = 50000;
    const uint32_t stun = 0x01020304u;
    const uint32_t wan_a = 0x0a000001u;
    const uint32_t wan_b = 0x0a000002u;
    const uint32_t lan_a = 0xc0a8010au;
    const uint32_t lan_b = 0xc0a8020bu;
    const uint16_t ext_a = a.outbound(lan_a, 10000, stun, 3478);
    const uint16_t ext_b = b.outbound(lan_b, 10000, stun, 3478);
    const uint16_t real_a = a.outbound(lan_a, 10000, wan_b, ext_b);
    const uint16_t real_b = b.outbound(lan_b, 10000, wan_a, ext_a);
    const bool a_hears = a.inbound(wan_b, real_b, ext_a);
    const bool b_hears = b.inbound(wan_a, real_a, ext_b);
    return a_hears && b_hears;
}

// 裸 STUN 口能通的充要条件：两端都不是 EDM（锥×锥）。
inline bool nat_sim_cone_pair(uint8_t map_a, uint8_t map_b) {
    return map_a != NAT_MAP_EDM && map_b != NAT_MAP_EDM;
}

} // namespace p2p

#endif // P2P_COMMON_NAT_SIM_H
