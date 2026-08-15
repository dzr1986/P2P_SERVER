// token_test.cpp：连线 Token 签发/验签单测
#include "common/ConnectToken.h"
#include "common/Uid.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace p2p;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    char dst[UID_LEN + 1], src[UID_LEN + 1], other[UID_LEN + 1];
    CHECK(uid_generate("CAMA", 'C', dst) == 0, "dst uid");
    CHECK(uid_generate("CAMA", 'C', src) == 0, "src uid");
    CHECK(uid_generate("CAMA", 'A', other) == 0, "other uid");

    uint8_t key[AUTH_KEY_LEN], key2[AUTH_KEY_LEN];
    uid_derive_auth_key((const uint8_t*)"master-secret-2026", 18, dst, key);
    uid_derive_auth_key((const uint8_t*)"master-secret-2026", 18, other, key2);

    ConnectToken tok{};
    CHECK(connect_token_issue(key, dst, src, 300, tok) == 0, "issue 300s");
    const uint32_t now = (uint32_t)time(nullptr);
    CHECK(connect_token_verify(key, dst, src, tok, now), "verify ok");
    CHECK(!connect_token_verify(key, dst, other, tok, now), "wrong src rejected");
    CHECK(!connect_token_verify(key2, dst, src, tok, now), "wrong key rejected");

    ConnectToken exp{};
    CHECK(connect_token_issue(key, dst, src, 0, exp) == 0, "issue expired");
    CHECK(!connect_token_verify(key, dst, src, exp, now), "expired rejected");

    ConnectToken bad = tok;
    bad.mac[0] ^= 0xFF;
    CHECK(!connect_token_verify(key, dst, src, bad, now), "tampered mac rejected");

    std::string hex = connect_token_to_hex(tok);
    CHECK(hex.size() == CONNECT_TOKEN_HEX_LEN, "hex length 104");
    ConnectToken back{};
    CHECK(connect_token_from_hex(hex, back) &&
              memcmp(&back, &tok, sizeof(tok)) == 0,
          "hex roundtrip");
    CHECK(!connect_token_from_hex("zz", back), "bad hex rejected");
    CHECK(!connect_token_from_hex("", back), "empty hex rejected");

    TokenNonceCache cache;
    CHECK(cache.claim(tok.nonce, now + 300, now), "nonce first claim");
    CHECK(!cache.claim(tok.nonce, now + 300, now), "nonce replay rejected");
    CHECK(cache.size() == 1, "cache holds one nonce");
    uint8_t other_n[16];
    memset(other_n, 0xAB, 16);
    CHECK(cache.claim(other_n, now + 10, now), "different nonce accepted");
    CHECK(cache.size() == 2, "cache holds two");
    cache.purge(now + 11);
    CHECK(cache.size() == 1, "expired nonce purged");
    CHECK(cache.claim(other_n, now + 100, now + 11), "expired slot reusable");

    if (g_fail == 0) { printf("token tests PASS\n"); return 0; }
    printf("token tests FAIL (%d)\n", g_fail);
    return 1;
}
