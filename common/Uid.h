#ifndef P2P_COMMON_UID_H
#define P2P_COMMON_UID_H

// 结构化设备 UID（对标 TUTK 20 字符 UID，见 docs/P2P服务器开发计划书.md 4.1）
//
//   UID(20 字符, Base32 大写) = PREFIX(4) + REGION(1) + RANDOM(12) + CRC(3)
//     PREFIX : 客户/产品线代码（签发时分配，用于计费与隔离）
//     REGION : 建议接入区域（调度提示，非强制）
//     RANDOM : 密码学随机（安全熵源，失败拒绝签发）
//     CRC    : 前 17 字符校验（SHA-256 截断 15bit -> 3 字符，快速拒绝手输错误/爆破）
//
// 每 UID 独立鉴权密钥（AuthKey）：
//   AuthKey = HMAC-SHA256(master_secret, uid)
//   设备烧录 AuthKey（不知晓 master）；服务端持 master 按需派生，无需逐设备存储。
//   单设备密钥泄露不影响其它设备（HMAC 不可逆推 master）。

#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

constexpr size_t UID_LEN = 20;          // 不含结尾 0
constexpr size_t UID_PREFIX_LEN = 4;
constexpr size_t UID_CRC_LEN = 3;
constexpr size_t AUTH_KEY_LEN = 32;

// 生成一个 UID 写入 out（容量 >= UID_LEN+1，NUL 结尾）
//   prefix: 4 字符（Base32 字符集）；region: 1 字符（Base32 字符集）
//   返回 0 成功 / -1 参数非法或安全熵源不可用
int uid_generate(const char* prefix, char region, char out[UID_LEN + 1]);

// 校验 UID：长度、字符集、CRC 全部通过返回 true
bool uid_valid(const char* uid);

// 按 master 密钥派生每 UID 独立 AuthKey（32 字节）
void uid_derive_auth_key(const uint8_t* master, size_t master_len,
                         const char* uid, uint8_t out[AUTH_KEY_LEN]);

// AuthKey 32 字节 <-> 64 字符小写 hex
std::string auth_key_to_hex(const uint8_t key[AUTH_KEY_LEN]);
bool auth_key_from_hex(const std::string& hex, uint8_t out[AUTH_KEY_LEN]);

} // namespace p2p

#endif // P2P_COMMON_UID_H
