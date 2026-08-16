// server_arch_test：guide 拆出的信令模块（同步 HMAC / 中继择优 / 挑战应答）
#include "server/config/CfgFile.h"
#include "server/management/AuthChallenge.h"
#include "server/management/RelayHealth.h"
#include "server/peers/RegistrySync.h"
#include "core/foundation/Crypto.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    CfgData off;
    AuthChallenge auth;
    uint8_t nonce[16] = {}, mac[32] = {};
    CHECK(auth.verify(off, "u", nonce, mac), "auth off = pass");

    CfgData on;
    on.enable_auth = true;
    on.auth_secret = "s3cr3t";
    CHECK(auth.issue("dev1", nonce), "issue nonce");
    CHECK(!auth.verify(on, "dev1", nonce, mac), "wrong mac rejected");

    CfgData priv;
    priv.private_mode = false;
    apply_cfg_env(priv);  // 无 P2P_PRIVATE_MODE 不改
    CHECK(!priv.private_mode, "unset PrivateMode keeps false");

    RelayHealth rh;
    CfgData cfg;
    cfg.proxy_ips = {"10.1.0.1", "10.1.0.2"};
    ProxyCandidate out[3]{};
    uint8_t n = 0;
    rh.pick(out, n, cfg, 18833, 0);
    CHECK(n >= 2, "pick falls back to config");
    CHECK(std::string(out[0].ip) == "10.1.0.1", "first fallback ip");
    CHECK(rh.available_count() == 0, "no health rows yet");

    RegistrySync sync;
    auto sc = std::make_shared<CfgData>();
    sc->sync_auth_secret = "sync-key";
    sync.set_cfg(sc);
    uint8_t body[4] = {1, 2, 3, 4};
    CHECK(!sync.verify(body, 4, 4), "signed required, short pkt fail");
    uint8_t pkt[36] = {};
    memcpy(pkt, body, 4);
    hmac_sha256((const uint8_t*)"sync-key", 8, body, 4, pkt + 4);
    CHECK(sync.verify(pkt, 36, 4), "hmac ok");
    pkt[4] ^= 1;
    CHECK(!sync.verify(pkt, 36, 4), "hmac tamper fail");

    sc->sync_auth_secret.clear();
    CHECK(sync.verify(body, 4, 4), "no secret = pass");

    if (g_fail) {
        printf("server_arch_test FAIL %d\n", g_fail);
        return 1;
    }
    printf("server_arch_test OK\n");
    return 0;
}
