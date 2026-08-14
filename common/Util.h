#ifndef P2P_COMMON_UTIL_H
#define P2P_COMMON_UTIL_H

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace p2p {

// ---------------------------------------------------------------------------
// 通用工具：地址编解码 / 时间戳 / 字节序辅助
// ---------------------------------------------------------------------------

// sockaddr_in -> "a.b.c.d:port"（线程安全，返回静态缓冲副本）
inline std::string addr_to_str(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%u", ip, (unsigned)ntohs(a.sin_port));
    return std::string(buf);
}

inline std::string ip_to_str(uint32_t addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    return std::string(ip);
}

inline std::string ipv4_to_str(const char* buf) {
    char ip[INET_ADDRSTRLEN];
    memset(ip, 0, sizeof(ip));
    // 兼容非 NUL 结尾的定长 IP 缓冲
    for (int i = 0; i < INET_ADDRSTRLEN - 1 && buf && buf[i]; i++) ip[i] = buf[i];
    return std::string(ip);
}

// 原版地址编码：u64 = (inet_addr(ip) << 32) | port
inline uint64_t addr_to_u64(const sockaddr_in& a) {
    return ((uint64_t)(uint32_t)a.sin_addr.s_addr << 32) | (uint64_t)ntohs(a.sin_port);
}

inline sockaddr_in u64_to_sockaddr(uint64_t v) {
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = (uint32_t)(v >> 32);
    a.sin_port = htons((uint16_t)(v & 0xFFFF));
    return a;
}

inline bool sockaddr_eq(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

inline uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

inline time_t now_sec() { return time(nullptr); }

// 生成随机 32 位（供会话号/挑战随机数使用，非加密级）
inline uint32_t rand_u32() {
    static uint64_t seed = now_ms() ^ (uint64_t)(uintptr_t)&seed;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(seed >> 33);
}

inline uint16_t rand_u16() { return (uint16_t)(rand_u32() >> 16); }

} // namespace p2p

#endif // P2P_COMMON_UTIL_H
