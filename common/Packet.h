#ifndef P2P_COMMON_PACKET_H
#define P2P_COMMON_PACKET_H

// 边界检查的报文读写器（C++ 风格重构基础设施）
//   替代散落各处的「长度判断 + memcpy + 手动截断」模式：
//   - PacketReader：越界读取返回 false，杜绝解析路径缓冲区溢出
//   - PacketWriter：越界写入返回 false，构包与容量检查合一
// 定长结构体仍走 memcpy（wire 格式 1 字节对齐，见 ProtoDef.h），
// 读写器负责的是「边界正确性」而非序列化格式本身。

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

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

} // namespace p2p

#endif // P2P_COMMON_PACKET_H
