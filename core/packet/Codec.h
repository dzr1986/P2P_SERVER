#ifndef P2P_SDK_PROTO_CODEC_H
#define P2P_SDK_PROTO_CODEC_H

// 协议编解码：XN 8B 头报文与 PT 隧道帧的构建/解析（无字节序适配层，手工 hton/ntoh）

#include <cstdint>
#include <cstring>
#include <string>

#include "core/packet/ProtoDef.h"

namespace p2p {

// 手工字节序（避开 glibc endian.h 的 htobe16/be16toh 宏命名冲突）
inline uint16_t p2p_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
inline uint32_t p2p_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

// ---- XN 报文头 ----
struct WireHead {
    uint16_t magic;
    uint8_t  version;
    uint8_t  msg_id;
    uint32_t length;
};

// 构建报文头到 buf（返回写入字节数 8）
inline int codec_write_head(uint8_t* buf, uint8_t msg_id, uint32_t payload_len) {
    buf[0] = (uint8_t)(NAT_MAGIC >> 8);
    buf[1] = (uint8_t)(NAT_MAGIC & 0xFF);
    buf[2] = PROTO_VER;
    buf[3] = msg_id;
    buf[4] = (uint8_t)(payload_len >> 24);
    buf[5] = (uint8_t)(payload_len >> 16);
    buf[6] = (uint8_t)(payload_len >> 8);
    buf[7] = (uint8_t)(payload_len & 0xFF);
    return 8;
}

// 解析报文头（输入 >=8B，返回 true；magic/版本不匹配返回 false）
inline bool codec_read_head(const uint8_t* buf, size_t len, WireHead& h) {
    if (len < 8) return false;
    h.magic   = (uint16_t)((buf[0] << 8) | buf[1]);
    h.version = buf[2];
    h.msg_id  = buf[3];
    h.length  = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
    if (h.magic != NAT_MAGIC) return false;
    if (h.version != PROTO_VER) return false;
    return true;
}

// ---- PT 隧道帧 ----
struct WireTunnel {
    uint8_t  type;
    uint16_t session_id;
    uint8_t  channel_id;
    uint8_t  flags;
    uint16_t seq;
    uint16_t ack;
    const uint8_t* payload;
    uint16_t len;
};

// 构建隧道帧（含负载），返回总字节数；>cap 返回 -1
inline int codec_write_tunnel(uint8_t* buf, int cap, uint8_t type, uint16_t session_id,
                              uint8_t channel_id, uint8_t flags, uint16_t seq,
                              uint16_t ack, const uint8_t* payload, uint16_t len) {
    if (8 + len > cap) return -1;
    buf[0] = (uint8_t)(TUNNEL_MAGIC >> 8);
    buf[1] = (uint8_t)(TUNNEL_MAGIC & 0xFF);
    buf[2] = TUNNEL_VER;
    buf[3] = type;
    buf[4] = (uint8_t)(session_id >> 8);
    buf[5] = (uint8_t)(session_id & 0xFF);
    buf[6] = channel_id;
    buf[7] = flags;
    buf[8] = (uint8_t)(seq >> 8);
    buf[9] = (uint8_t)(seq & 0xFF);
    buf[10] = (uint8_t)(ack >> 8);
    buf[11] = (uint8_t)(ack & 0xFF);
    buf[12] = (uint8_t)(len >> 8);
    buf[13] = (uint8_t)(len & 0xFF);
    if (len > 0 && payload) memcpy(buf + 14, payload, len);
    return 14 + len;
}

// 解析隧道帧（输入 >=14B），payload/len 指向帧内数据
inline bool codec_read_tunnel(const uint8_t* buf, size_t len, WireTunnel& t) {
    if (len < 14) return false;
    if (buf[0] != (uint8_t)(TUNNEL_MAGIC >> 8) || buf[1] != (uint8_t)(TUNNEL_MAGIC & 0xFF))
        return false;
    if (buf[2] != TUNNEL_VER) return false;
    t.type       = buf[3];
    t.session_id = (uint16_t)((buf[4] << 8) | buf[5]);
    t.channel_id = buf[6];
    t.flags      = buf[7];
    t.seq        = (uint16_t)((buf[8] << 8) | buf[9]);
    t.ack        = (uint16_t)((buf[10] << 8) | buf[11]);
    t.len        = (uint16_t)((buf[12] << 8) | buf[13]);
    if ((size_t)14 + t.len > len) return false;
    t.payload = buf + 14;
    return true;
}

} // namespace p2p

#endif // P2P_SDK_PROTO_CODEC_H
