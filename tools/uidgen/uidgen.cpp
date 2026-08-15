// uidgen：UID 批量签发工具（计划书 P1）
//   用法：uidgen <PREFIX4> <REGION> <count> <master_secret>
//   输出：每行 "UID AUTHKEY_HEX"
//     - UID     烧录到设备（20 字符 Base32，含 CRC）
//     - AuthKey 烧录到设备安全存储（HMAC(master, uid) 派生，服务端持 master 即可验证）
//   master_secret 即服务端 P2pServers.cfg 的 AuthSecret。
#include "common/Uid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace p2p;

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <PREFIX4> <REGION> <count> <master_secret>\n"
                "  PREFIX4: 4 chars, REGION: 1 char (charset A-Z 2-7)\n"
                "  output: one \"UID AUTHKEY_HEX\" per line\n",
                argv[0]);
        return 1;
    }
    const char* prefix = argv[1];
    const char region = argv[2][0];
    const int count = atoi(argv[3]);
    const char* master = argv[4];
    if (count <= 0 || count > 1000000) {
        fprintf(stderr, "bad count\n");
        return 1;
    }
    if (strlen(master) < 8) {
        fprintf(stderr, "master_secret too short (>=8 required)\n");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        char uid[UID_LEN + 1];
        if (uid_generate(prefix, region, uid) != 0) {
            fprintf(stderr, "uid_generate failed (bad prefix/region or no entropy)\n");
            return 1;
        }
        uint8_t key[AUTH_KEY_LEN];
        uid_derive_auth_key(reinterpret_cast<const uint8_t*>(master), strlen(master),
                            uid, key);
        printf("%s %s\n", uid, auth_key_to_hex(key).c_str());
    }
    return 0;
}
