// tokengen：连线 Token 签发（计划书 P1）
//   用法：tokengen <dst_uid> <src_uid> <ttl_sec> <master_secret>
//   输出：一行 104 字符 hex（ConnectToken）
//   Token 用 dst 的 AuthKey 签名，授权 src 连接 dst；ttl_sec=0 表示已过期（测试用）。
#include "core/foundation/ConnectToken.h"
#include "core/foundation/Uid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace p2p;

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <dst_uid> <src_uid> <ttl_sec> <master_secret>\n"
                "  output: 104-hex ConnectToken (HMAC with dst AuthKey)\n",
                argv[0]);
        return 1;
    }
    const char* dst = argv[1];
    const char* src = argv[2];
    const long ttl = atol(argv[3]);
    const char* master = argv[4];
    if (!dst[0] || !src[0] || ttl < 0 || ttl > 7 * 24 * 3600) {
        fprintf(stderr, "bad dst/src/ttl\n");
        return 1;
    }
    if (strlen(master) < 8) {
        fprintf(stderr, "master_secret too short (>=8 required)\n");
        return 1;
    }
    uint8_t key[AUTH_KEY_LEN];
    uid_derive_auth_key(reinterpret_cast<const uint8_t*>(master), strlen(master),
                        dst, key);
    ConnectToken tok{};
    if (connect_token_issue(key, dst, src, (uint32_t)ttl, tok) != 0) {
        fprintf(stderr, "issue failed\n");
        return 1;
    }
    printf("%s\n", connect_token_to_hex(tok).c_str());
    return 0;
}
