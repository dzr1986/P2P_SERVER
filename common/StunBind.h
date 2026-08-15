#ifndef P2P_COMMON_STUN_BIND_H
#define P2P_COMMON_STUN_BIND_H

// 最小 RFC 8489 STUN Binding（供 NatServer 兼做 STUN，libjuice 收集 srflx）
// 仅处理无鉴权的 Binding Request → Success + XOR-MAPPED-ADDRESS。

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>

namespace p2p {

constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442u;
constexpr uint16_t STUN_BINDING_REQUEST = 0x0001;
constexpr uint16_t STUN_BINDING_SUCCESS = 0x0101;
constexpr uint16_t STUN_ATTR_XOR_MAPPED = 0x0020;
constexpr size_t   STUN_HDR_LEN = 20;
constexpr size_t   STUN_BINDING_SUCCESS_LEN = 32;  // header + XOR-MAPPED-ADDRESS(IPv4)

// 20 字节 Binding Request（无属性）。tid 可空（填递增占位）。
inline size_t stun_write_binding_request(uint8_t* out, size_t cap,
                                         const uint8_t tid[12] = nullptr) {
    if (!out || cap < STUN_HDR_LEN) return 0;
    memset(out, 0, STUN_HDR_LEN);
    out[1] = (uint8_t)(STUN_BINDING_REQUEST & 0xFF);
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42;
    if (tid) memcpy(out + 8, tid, 12);
    else {
        for (int i = 0; i < 12; i++) out[8 + i] = (uint8_t)(0xC0 + i);
    }
    return STUN_HDR_LEN;
}

inline bool stun_is_binding_request(const uint8_t* data, size_t len) {
    if (!data || len < STUN_HDR_LEN) return false;
    const uint16_t type = (uint16_t)((data[0] << 8) | data[1]);
    const uint32_t magic = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                           ((uint32_t)data[6] << 8) | data[7];
    return type == STUN_BINDING_REQUEST && magic == STUN_MAGIC_COOKIE;
}

// 写入 32 字节 Binding Success；成功返回 32，非法请求返回 0
inline size_t stun_write_binding_success(uint8_t* out, size_t cap,
                                         const uint8_t* req, size_t reqlen,
                                         const sockaddr_in& mapped) {
    if (!out || cap < STUN_BINDING_SUCCESS_LEN) return 0;
    if (!stun_is_binding_request(req, reqlen)) return 0;
    memset(out, 0, STUN_BINDING_SUCCESS_LEN);
    out[0] = (uint8_t)(STUN_BINDING_SUCCESS >> 8);
    out[1] = (uint8_t)(STUN_BINDING_SUCCESS & 0xFF);
    out[2] = 0;
    out[3] = 12;                              // 属性体长度
    memcpy(out + 4, req + 4, 16);             // magic + transaction id
    out[20] = (uint8_t)(STUN_ATTR_XOR_MAPPED >> 8);
    out[21] = (uint8_t)(STUN_ATTR_XOR_MAPPED & 0xFF);
    out[22] = 0;
    out[23] = 8;
    out[24] = 0;
    out[25] = 0x01;                           // IPv4
    const uint16_t xport = mapped.sin_port ^ htons(0x2112);
    memcpy(out + 26, &xport, 2);
    const uint32_t xaddr = mapped.sin_addr.s_addr ^ htonl(STUN_MAGIC_COOKIE);
    memcpy(out + 28, &xaddr, 4);
    return STUN_BINDING_SUCCESS_LEN;
}

// 解析 Success 中的 XOR-MAPPED IPv4；失败返回 false
inline bool stun_read_xor_mapped(const uint8_t* rsp, size_t len,
                                 uint32_t* addr_be, uint16_t* port_be) {
    if (!rsp || len < STUN_BINDING_SUCCESS_LEN) return false;
    const uint16_t type = (uint16_t)((rsp[0] << 8) | rsp[1]);
    if (type != STUN_BINDING_SUCCESS) return false;
    uint16_t xport;
    uint32_t xaddr;
    memcpy(&xport, rsp + 26, 2);
    memcpy(&xaddr, rsp + 28, 4);
    if (port_be) *port_be = xport ^ htons(0x2112);
    if (addr_be) *addr_be = xaddr ^ htonl(STUN_MAGIC_COOKIE);
    return true;
}

inline bool stun_reply_binding(int fd, const uint8_t* req, size_t reqlen,
                               const sockaddr_in& from) {
    uint8_t rsp[STUN_BINDING_SUCCESS_LEN];
    if (stun_write_binding_success(rsp, sizeof(rsp), req, reqlen, from) == 0)
        return false;
    sendto(fd, rsp, sizeof(rsp), 0,
           reinterpret_cast<const sockaddr*>(&from), sizeof(from));
    return true;
}

} // namespace p2p

#endif // P2P_COMMON_STUN_BIND_H
