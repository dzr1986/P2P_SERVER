#ifndef P2P_SDK_PLAT_H
#define P2P_SDK_PLAT_H

// ---------------------------------------------------------------------------
// 平台抽象层：客户端 SDK 对平台差异的隔离点（Linux 用 std；Android/iOS/Windows
// 移植时仅需替换本文件 + 线程/时间封装）
// ---------------------------------------------------------------------------
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace p2p {

inline uint64_t plat_now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void plat_sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 随机字节（/dev/urandom 优先，失败回退 xorshift 伪随机）
inline void plat_rand_bytes(uint8_t* out, size_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
    static std::atomic<uint64_t> counter{0x9E3779B97F4A7C15ULL};
    uint64_t seed = (uint64_t)plat_now_ms() ^ counter.fetch_add(0x9E3779B97F4A7C15ULL);
    for (size_t i = 0; i < n; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        out[i] = (uint8_t)(seed & 0xFF);
    }
}

} // namespace p2p

#endif // P2P_SDK_PLAT_H
