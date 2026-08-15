#ifndef P2P_COMMON_CONNECT_TOKEN_H
#define P2P_COMMON_CONNECT_TOKEN_H

// 连线 Token（计划书 P1）：业主用设备 AuthKey 签发，客户端 CONNECT 时出示。
// NatServer 用 master 派生 dst AuthKey 验签后才下发 CONNECT_ACK。

#include "ProtoDef.h"
#include "Uid.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

constexpr size_t CONNECT_TOKEN_HEX_LEN = sizeof(ConnectToken) * 2;  // 104

// 签发：ttl_sec=0 表示立即过期（测试用）。成功返回 0。
int connect_token_issue(const uint8_t auth_key[AUTH_KEY_LEN],
                        const char* dst_uid, const char* src_uid,
                        uint32_t ttl_sec, ConnectToken& out);

// 验签：过期、MAC 错误、参数非法返回 false。
bool connect_token_verify(const uint8_t auth_key[AUTH_KEY_LEN],
                          const char* dst_uid, const char* src_uid,
                          const ConnectToken& tok, uint32_t now_unix);

std::string connect_token_to_hex(const ConnectToken& tok);
bool connect_token_from_hex(const std::string& hex, ConnectToken& out);

} // namespace p2p

#endif // P2P_COMMON_CONNECT_TOKEN_H
