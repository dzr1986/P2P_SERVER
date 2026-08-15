#ifndef P2P_SDK_PLAT_H
#define P2P_SDK_PLAT_H

// ---------------------------------------------------------------------------
// 平台抽象层：客户端 SDK 对平台差异的隔离点（Linux 用 std；Android/iOS/Windows
// 移植时仅需替换本文件 + 线程/时间封装）
// ---------------------------------------------------------------------------
#include "common/Crypto.h"

#include <chrono>
#include <cstdint>
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

// 随机字节：走平台安全熵源；失败时返回 false，调用方应拒绝继续
inline bool plat_rand_bytes(uint8_t* out, size_t n) {
    return p2p_random_bytes(out, n) == 0;
}

} // namespace p2p

#endif // P2P_SDK_PLAT_H
