#ifndef P2P_SDK_IOTC_TUNNEL_CODEC_H
#define P2P_SDK_IOTC_TUNNEL_CODEC_H

// P2PTunnel 控制/数据帧编解码（计划书 P4，头文件实现便于单测）
//   布局（6 字节头）：
//     magic(1)=0xA2 | type(1) | conn_id(2 BE) | len(2 BE) | payload
//   type: OPEN / OPEN_ACK / DATA / CLOSE

#include <cstdint>
#include <cstring>

#include "core/packet/ProtoDef.h"

namespace p2p {

constexpr uint8_t TUN_MAGIC = 0xA2;
constexpr size_t  TUN_HDR = 6;

enum TunType : uint8_t {
    TUN_OPEN     = 1,
    TUN_OPEN_ACK = 2,
    TUN_DATA     = 3,
    TUN_CLOSE    = 4,
};

constexpr size_t TUN_MAX_PAYLOAD = MAX_TUNNEL_PAYLOAD - TUN_HDR;

inline size_t tun_write(uint8_t* out, size_t cap, uint8_t type, uint16_t conn_id,
                        const uint8_t* payload, size_t plen) {
    if (!out || cap < TUN_HDR + plen || plen > TUN_MAX_PAYLOAD) return 0;
    if (plen > 0 && !payload) return 0;
    out[0] = TUN_MAGIC;
    out[1] = type;
    out[2] = (uint8_t)(conn_id >> 8);
    out[3] = (uint8_t)(conn_id & 0xFF);
    out[4] = (uint8_t)(plen >> 8);
    out[5] = (uint8_t)(plen & 0xFF);
    if (plen > 0) memcpy(out + TUN_HDR, payload, plen);
    return TUN_HDR + plen;
}

inline bool tun_read(const uint8_t* buf, size_t len, uint8_t& type,
                     uint16_t& conn_id, const uint8_t*& payload, size_t& plen) {
    if (!buf || len < TUN_HDR || buf[0] != TUN_MAGIC) return false;
    type = buf[1];
    conn_id = (uint16_t)((buf[2] << 8) | buf[3]);
    plen = (size_t)((buf[4] << 8) | buf[5]);
    if (TUN_HDR + plen != len) return false;
    payload = (plen > 0) ? buf + TUN_HDR : nullptr;
    return type >= TUN_OPEN && type <= TUN_CLOSE;
}

} // namespace p2p

#endif // P2P_SDK_IOTC_TUNNEL_CODEC_H
