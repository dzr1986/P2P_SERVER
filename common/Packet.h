#ifndef P2P_COMMON_PACKET_H
#define P2P_COMMON_PACKET_H

// 边界检查的报文读写器（C++ 风格重构基础设施）
//   替代散落各处的「长度判断 + memcpy + 手动截断」模式：
//   - PacketReader：越界读取返回 false，杜绝解析路径缓冲区溢出
//   - PacketWriter：越界写入返回 false，构包与容量检查合一
// 定长结构体仍走 memcpy（wire 格式 1 字节对齐，见 ProtoDef.h），
// 读写器负责的是「边界正确性」而非序列化格式本身。

#include "ProtoDef.h"

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace p2p {

class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    size_t remaining() const { return (size_t)(end_ - p_); }
    const uint8_t* cursor() const { return p_; }

    // 读取定长 wire 结构体（trivially copyable），越界返回 false
    template <typename T>
    bool read_struct(T& out) {
        static_assert(std::is_trivially_copyable<T>::value, "wire struct required");
        if (remaining() < sizeof(T)) return false;
        memcpy(&out, p_, sizeof(T));
        p_ += sizeof(T);
        return true;
    }

    bool read_bytes(void* dst, size_t n) {
        if (remaining() < n) return false;
        memcpy(dst, p_, n);
        p_ += n;
        return true;
    }

    bool skip(size_t n) {
        if (remaining() < n) return false;
        p_ += n;
        return true;
    }

    // 剩余字节转 string（变长尾部字段，如扩展信息）
    bool read_rest(std::string& out, size_t max_len) {
        size_t n = remaining();
        if (n > max_len) return false;
        out.assign(reinterpret_cast<const char*>(p_), n);
        p_ = end_;
        return true;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

class PacketWriter {
public:
    PacketWriter(uint8_t* buf, size_t cap) : begin_(buf), p_(buf), end_(buf + cap) {}

    size_t size() const { return (size_t)(p_ - begin_); }
    size_t remaining() const { return (size_t)(end_ - p_); }
    const uint8_t* data() const { return begin_; }

    template <typename T>
    bool write_struct(const T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "wire struct required");
        if (remaining() < sizeof(T)) return false;
        memcpy(p_, &v, sizeof(T));
        p_ += sizeof(T);
        return true;
    }

    bool write_bytes(const void* src, size_t n) {
        if (remaining() < n) return false;
        if (n > 0) memcpy(p_, src, n);
        p_ += n;
        return true;
    }

private:
    uint8_t* begin_;
    uint8_t* p_;
    uint8_t* end_;
};

// 定长 char 数组字段（补零、可能无 NUL 结尾）安全转 string
template <size_t N>
inline std::string wire_str(const char (&field)[N]) {
    return std::string(field, strnlen(field, N));
}

// 构造完整 XN 报文（MsgHead + payload）到 buf，返回总长；容量不足返回 0
// 统一服务端各处手写 MsgHead 的模式（客户端 SDK 用 proto/Codec.h 等价实现）
// 从 TCP 流缓冲弹出一帧完整 XN 报文。
// 返回：>0 已消费字节并填好 msg_id/payload；0 数据不够；-1 非法帧（调用方应断开）。
inline int xn_pop_frame(std::vector<uint8_t>& buf, uint8_t& msg_id,
                        std::vector<uint8_t>& payload) {
    if (buf.size() < sizeof(MsgHead)) return 0;
    MsgHead h{};
    memcpy(&h, buf.data(), sizeof(MsgHead));
    if (ntohs(h.magic) != NAT_MAGIC || h.version != PROTO_VER) return -1;
    const uint32_t plen = ntohl(h.length);
    if (plen > (uint32_t)MAX_PKT) return -1;
    const size_t need = sizeof(MsgHead) + plen;
    if (buf.size() < need) return 0;
    msg_id = h.msg_id;
    payload.assign(buf.begin() + sizeof(MsgHead), buf.begin() + (ptrdiff_t)need);
    buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)need);
    return (int)need;
}

inline size_t build_msg(uint8_t* buf, size_t cap, uint8_t msg_id,
                        const void* payload, size_t plen) {
    PacketWriter w(buf, cap);
    MsgHead h{};
    h.magic = htons(NAT_MAGIC);
    h.version = PROTO_VER;
    h.msg_id = msg_id;
    h.length = htonl((uint32_t)plen);
    if (!w.write_struct(h) || !w.write_bytes(payload, plen)) return 0;
    return w.size();
}

} // namespace p2p

#endif // P2P_COMMON_PACKET_H
