// server_arch_test：guide 拆出的信令模块（同步 HMAC / 中继择优 / 挑战应答）
#include "server/config/CfgFile.h"
#include "server/management/AntiAbuse.h"
#include "server/management/AuthChallenge.h"
#include "server/management/ConnectAuth.h"
#include "server/management/RelayHealth.h"
#include "server/peers/PeerManage.h"
#include "server/peers/RegistrySync.h"
#include "core/connectivity/hole_punch/PunchAdmit.h"
#include "core/foundation/Crypto.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

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

    PeerManager pm;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    CHECK(pm.upsert("dev-need", a, a, 1, NAT_FULL_CONE, "np=1"), "upsert need");
    Peer got;
    CHECK(pm.get("dev-need", got) && got.need_p2p, "extinfo np=1 sets need_p2p");
    CHECK(pm.need_p2p_count() == 1, "need_p2p_count");
    CHECK(pm.upsert("dev-need", a, a, 1, NAT_FULL_CONE, ""), "upsert clear");
    CHECK(pm.get("dev-need", got) && !got.need_p2p, "empty extinfo clears need_p2p");

    CfgData tok_off;
    TokenNonceCache nonces;
    CHECK(verify_connect_trailer(tok_off, nonces, "s", "d", nullptr, 0),
          "token off = pass");
    CfgData tok_on;
    tok_on.enable_connect_token = true;
    CHECK(!verify_connect_trailer(tok_on, nonces, "s", "d", nullptr, 0),
          "token on + empty secret fail");
    tok_on.auth_secret = "s3cr3t";
    CHECK(!verify_connect_trailer(tok_on, nonces, "s", "d", nullptr, 0),
          "token on + no trailer fail");

    AntiAbuse jail;
    jail.configure(1, 60);
    jail.set_jail_path("/tmp/p2p_jail_test.txt");
    sockaddr_in flood{};
    flood.sin_family = AF_INET;
    inet_pton(AF_INET, "203.0.113.9", &flood.sin_addr);
    jail.check_flood(flood);
    jail.check_flood(flood);
    CHECK(jail.dump_jail(), "dump_jail writes");
    FILE* jf = fopen("/tmp/p2p_jail_test.txt", "r");
    CHECK(jf != nullptr, "jail file exists");
    char line[64] = {};
    if (jf) {
        CHECK(fgets(line, sizeof(line), jf) != nullptr, "jail has a line");
        fclose(jf);
    }
    CHECK(std::string(line).find("203.0.113.9") != std::string::npos, "jail ip");
    unlink("/tmp/p2p_jail_test.txt");

    if (g_fail) {
        printf("server_arch_test FAIL %d\n", g_fail);
        return 1;
    }
    printf("server_arch_test OK\n");
    return 0;
}
