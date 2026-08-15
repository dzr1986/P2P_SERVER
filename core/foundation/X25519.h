#ifndef P2P_COMMON_X25519_H
#define P2P_COMMON_X25519_H

// X25519 ECDH（RFC 7748），用于 P5 前向保密会话密钥协商
//   实现为 Montgomery ladder，无外部依赖；密钥对用 p2p_random_bytes 生成。

#include <cstddef>
#include <cstdint>

namespace p2p {

constexpr size_t X25519_KEY_LEN = 32;

// out = scalar * point（RFC 7748 X25519）
// 返回 0 成功 / -1 参数非法
int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

// 生成临时密钥对：priv 来自安全随机并 clamp，pub = priv * 9
// 返回 0 成功 / -1 无熵源
int x25519_keypair(uint8_t pub[32], uint8_t priv[32]);

} // namespace p2p

#endif // P2P_COMMON_X25519_H
