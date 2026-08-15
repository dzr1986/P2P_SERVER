// uid_test.cpp：结构化 UID（P1）单测
//   - 生成/校验/CRC 防篡改
//   - AuthKey 派生确定性与设备隔离
#include "common/Uid.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace p2p;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    // 1. 生成合法 UID
    char uid[UID_LEN + 1];
    CHECK(uid_generate("CAMA", 'C', uid) == 0, "generate ok");
    CHECK(strlen(uid) == UID_LEN, "length is 20");
    CHECK(uid_valid(uid), "generated uid passes validation");
    CHECK(strncmp(uid, "CAMA", 4) == 0 && uid[4] == 'C', "prefix/region preserved");
    CHECK(uid_region(uid) == 'C', "uid_region extracts C");
    CHECK(uid_region("BADUID") == 0, "uid_region rejects unstructured");
    CHECK(uid_region(nullptr) == 0, "uid_region null");

    // 2. 篡改任一字符 CRC 必失败
    bool tamper_all_rejected = true;
    for (size_t i = 0; i < UID_LEN; i++) {
        char t[UID_LEN + 1];
        memcpy(t, uid, sizeof(t));
        t[i] = (t[i] == 'A') ? 'B' : 'A';
        if (t[i] != uid[i] && uid_valid(t)) { tamper_all_rejected = false; break; }
    }
    CHECK(tamper_all_rejected, "any single-char tamper rejected by CRC");

    // 3. 非法输入
    CHECK(!uid_valid(nullptr), "null rejected");
    CHECK(!uid_valid(""), "empty rejected");
    CHECK(!uid_valid("SHORT"), "short rejected");
    CHECK(!uid_valid("abcdefghijklmnopqrst"), "lowercase rejected");
    CHECK(uid_generate("bad!", 'C', uid) != 0, "bad prefix rejected");
    CHECK(uid_generate("CAMA", '!', uid) != 0, "bad region rejected");

    // 4. 唯一性冒烟（1000 个无重复）
    std::set<std::string> seen;
    bool dup = false;
    for (int i = 0; i < 1000; i++) {
        char u[UID_LEN + 1];
        if (uid_generate("CAMA", 'C', u) != 0) { dup = true; break; }
        if (!seen.insert(u).second) { dup = true; break; }
    }
    CHECK(!dup, "1000 uids unique");

    // 5. AuthKey 派生：确定性 + 不同 UID 不同 key + 不同 master 不同 key
    char u1[UID_LEN + 1], u2[UID_LEN + 1];
    uid_generate("CAMA", 'C', u1);
    uid_generate("CAMA", 'C', u2);
    uint8_t k1a[AUTH_KEY_LEN], k1b[AUTH_KEY_LEN], k2[AUTH_KEY_LEN], k1m[AUTH_KEY_LEN];
    uid_derive_auth_key((const uint8_t*)"master-1", 8, u1, k1a);
    uid_derive_auth_key((const uint8_t*)"master-1", 8, u1, k1b);
    uid_derive_auth_key((const uint8_t*)"master-1", 8, u2, k2);
    uid_derive_auth_key((const uint8_t*)"master-2", 8, u1, k1m);
    CHECK(memcmp(k1a, k1b, AUTH_KEY_LEN) == 0, "derivation deterministic");
    CHECK(memcmp(k1a, k2, AUTH_KEY_LEN) != 0, "different uid -> different key");
    CHECK(memcmp(k1a, k1m, AUTH_KEY_LEN) != 0, "different master -> different key");

    // 6. hex 往返
    std::string hex = auth_key_to_hex(k1a);
    uint8_t back[AUTH_KEY_LEN];
    CHECK(hex.size() == 64 && auth_key_from_hex(hex, back) &&
              memcmp(back, k1a, AUTH_KEY_LEN) == 0,
          "authkey hex roundtrip");
    CHECK(!auth_key_from_hex("zz", back), "bad hex rejected");

    if (g_fail == 0) { printf("uid tests PASS\n"); return 0; }
    printf("uid tests FAIL (%d)\n", g_fail);
    return 1;
}
