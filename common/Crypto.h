#ifndef P2P_COMMON_CRYPTO_H
#define P2P_COMMON_CRYPTO_H

#include <cstddef>
#include <cstdint>

namespace p2p {

// ---------------------------------------------------------------------------
// 自研 SHA-256 / HMAC-SHA256（无外部依赖）
// 用途：鉴权挑战应答、UUID 校验、载荷完整性
// ---------------------------------------------------------------------------

constexpr size_t SHA256_DIGEST_LEN = 32;

// SHA-256 单次计算，out 长度 32
void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);
void sha256_hex(const uint8_t* data, size_t len, char out_hex[65]);

// HMAC-SHA256(key, msg) -> out 32 字节
void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256：口令派生（License 文件加密密钥等）
//   out_len 任意；推导强度由 iterations 控制（>=1000 起）
//   salt_len 必须 <= 60（HMAC-SHA256 block size 64B 减去 4B block counter）
//   返回 false 且不写 out 表示 salt 过长（参数错误），调用方必须显式处理，
//   不应静默使用未初始化/全零密钥
// ---------------------------------------------------------------------------
bool pbkdf2_hmac_sha256(const uint8_t* pw, size_t pw_len,
                        const uint8_t* salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t* out, size_t out_len);

// ---------------------------------------------------------------------------
// AES-256-CBC（自研，PKCS7 填充，iv 16B）
//   返回 0 成功 / -1 失败（缓冲区不足、长度非法、填充错误）
// ---------------------------------------------------------------------------
int aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len);
int aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len);

// ---------------------------------------------------------------------------
// 密码学安全随机（仅使用 /dev/urandom；无降级方案）
//   若 /dev/urandom 不可用或读取不足 len 字节，填充全零并返回，调用方应
//   视为熵源不可用，拒绝继续（不做 xorshift 等弱 PRNG 降级）
// ---------------------------------------------------------------------------
void p2p_random_bytes(uint8_t* out, size_t len);

// ---------------------------------------------------------------------------
// 心跳载荷流加密（XOR 对称，加密/解密同一函数）
//   密钥流：keystream_block(i) = HMAC-SHA256(secret, uuid(33B补零)||iv(8)||i)
//   uuid 必须为 MAX_UUID_LEN+1=33 字节的定长缓冲区（不足补零）
//   就地加密/解密 data[0..len)；len 建议 <= 4096
// ---------------------------------------------------------------------------
void p2p_stream_xor(const uint8_t* secret, size_t secret_len,
                    const char* uuid, const uint8_t iv[8],
                    uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// AEAD 加密接口（AES-256-CTR + HMAC-SHA256，提供机密性 + 完整性保护）
//    nonce: 12 字节随机数（每次加密必须唯一）
//   tag: 16 字节认证标签（解密时校验，防止篡改）
//   返回 0 成功 / -1 失败（缓冲区不足、tag 校验失败）
// ---------------------------------------------------------------------------
int p2p_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap, size_t* out_len,
                     uint8_t tag[16]);
int p2p_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap, size_t* out_len,
                     const uint8_t tag[16]);

// ---------------------------------------------------------------------------
// 加密器抽象层（trait 抽象，支持运行时切换算法）
//   参考 EasyTier Encryptor trait 设计
// ---------------------------------------------------------------------------
enum class EncryptionAlgorithm {
    Xor,        // 流密码（兼容旧协议，无完整性保护）
    Aes256Ctr,  // AES-256-CTR + HMAC-SHA256（AEAD）
};

class Encryptor {
public:
    virtual ~Encryptor() = default;
    // 加密：返回 0 成功 / -1 失败
    virtual int encrypt(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap, size_t* out_len) = 0;
    // 解密：返回 0 成功 / -1 失败（含 tag 校验失败）
    virtual int decrypt(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap, size_t* out_len) = 0;
    virtual EncryptionAlgorithm algorithm() const = 0;
};

// 工厂函数：创建加密器（key 32 字节；Xor 模式使用 uuid+iv 作为上下文）
// 返回 nullptr 表示不支持的算法
Encryptor* create_encryptor(EncryptionAlgorithm alg,
                            const uint8_t key[32],
                            const char* uuid = nullptr, const uint8_t iv[8] = nullptr);

} // namespace p2p

#endif // P2P_COMMON_CRYPTO_H
