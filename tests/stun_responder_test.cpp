// stun_responder_test：CHANGE-REQUEST / OTHER-ADDRESS / 映射观察 / 心跳不覆盖 nattype
#include "core/packet/StunBind.h"
#include "server/connectivity/StunResponder.h"
#include "server/connectivity/MappingObserve.h"
#include "server/peers/PeerManage.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

static sockaddr_in addr(const char* ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

int main() {
    uint8_t tid[12];
    for (int i = 0; i < 12; i++) tid[i] = (uint8_t)(0xB0 + i);

    uint8_t req[28];
    CHECK(stun_write_binding_request_change(req, sizeof(req), tid, false, true) == 28,
          "write CHANGE-REQUEST 28B");
    bool cip = true, cport = false;
    CHECK(stun_parse_change_request(req, sizeof(req), &cip, &cport), "parse change");
    CHECK(!cip && cport, "change-port only");

    uint8_t plain[20];
    CHECK(stun_write_binding_request(plain, sizeof(plain), tid) == 20, "plain request");
    CHECK(!stun_parse_change_request(plain, sizeof(plain), &cip, &cport),
          "no CHANGE-REQUEST");
    CHECK(!cip && !cport, "flags cleared");

    sockaddr_in mapped = addr("203.0.113.7", 4433);
    sockaddr_in other = addr("203.0.113.8", 3479);
    StunReplyPlan same = stun_plan_reply(plain, sizeof(plain), mapped, &other, true);
    CHECK(same.src == StunSendSrc::Same && same.len == 44, "plain → same + OTHER");
    uint32_t oip = 0;
    uint16_t oport = 0;
    CHECK(stun_read_other_address(same.buf, same.len, &oip, &oport), "read OTHER");
    CHECK(ntohs(oport) == 3479, "OTHER port");
    char ip[16] = {0};
    inet_ntop(AF_INET, &oip, ip, sizeof(ip));
    CHECK(strcmp(ip, "203.0.113.8") == 0, "OTHER ip");

    StunReplyPlan chg = stun_plan_reply(req, sizeof(req), mapped, &other, true);
    CHECK(chg.src == StunSendSrc::Other && chg.len == 44, "CHANGE → other socket");
    StunReplyPlan nochg = stun_plan_reply(req, sizeof(req), mapped, nullptr, false);
    CHECK(nochg.src == StunSendSrc::Same && nochg.len == 32,
          "no alt socket stays same 32B");

    MappingObserve obs(90);
    sockaddr_in a1 = addr("198.51.100.9", 50000);
    sockaddr_in a2 = addr("198.51.100.9", 50001);
    CHECK(obs.inferred_nat(a1) == NAT_UNKNOWN, "observe empty");
    obs.note(a1, 0);
    CHECK(obs.inferred_nat(a1) == NAT_UNKNOWN, "only main still unknown");
    obs.note(a1, 1);
    CHECK(obs.inferred_nat(a1) == NAT_PORT_RESTRICTED, "same port → cone/PRC");
    MappingObserve edm(90);
    edm.note(a1, 0);
    edm.note(a2, 1);
    CHECK(edm.inferred_nat(a1) == NAT_SYMMETRIC, "diff port → EDM");

    PeerManager pm;
    sockaddr_in pub = addr("198.51.100.9", 60000);
    sockaddr_in lan = addr("192.168.1.8", 60000);
    CHECK(pm.upsert("UIDA", pub, lan, 1, NAT_SYMMETRIC, ""), "first upsert");
    CHECK(pm.upsert("UIDA", pub, lan, 1, NAT_UNKNOWN, ""), "heartbeat 0");
    Peer p;
    CHECK(pm.get("UIDA", p) && p.nattype == NAT_SYMMETRIC, "keep known nattype");
    CHECK(pm.upsert("UIDA", pub, lan, 1, NAT_PORT_RESTRICTED, ""), "detect done");
    CHECK(pm.get("UIDA", p) && p.nattype == NAT_PORT_RESTRICTED, "non-zero updates");
    pm.set_nattype("UIDA", NAT_SYMMETRIC);
    CHECK(pm.get("UIDA", p) && p.nattype == NAT_SYMMETRIC, "set_nattype");

    if (g_fail) {
        printf("stun_responder_test: %d FAIL\n", g_fail);
        return 1;
    }
    printf("stun_responder_test: all ok\n");
    return 0;
}
