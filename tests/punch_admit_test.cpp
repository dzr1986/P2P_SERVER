#include "core/connectivity/hole_punch/PunchAdmit.h"
#include "server/listener/LocalListeners.h"

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
    CHECK(admit_connect_punch(NAT_FULL_CONE, NAT_FULL_CONE) == PunchAdmit::Ice,
          "cone×cone → ice");
    CHECK(admit_connect_punch(NAT_FULL_CONE, NAT_PORT_RESTRICTED) == PunchAdmit::Ice,
          "cone×prc → ice");
    CHECK(admit_connect_punch(NAT_SYMMETRIC, NAT_FULL_CONE) == PunchAdmit::Ice,
          "EDM×EIM → ice (predict/birthday 在客户端)");
    CHECK(admit_connect_punch(NAT_SYMMETRIC, NAT_SYMMETRIC) == PunchAdmit::Relay,
          "EDM×EDM → relay hint");
    CHECK(admit_connect_punch(NAT_UNKNOWN, NAT_FULL_CONE) == PunchAdmit::Unknown,
          "unknown → unknown");
    CHECK(resolve_nattype(NAT_UNKNOWN, NAT_SYMMETRIC) == NAT_SYMMETRIC,
          "observe fills unknown");
    CHECK(resolve_nattype(NAT_FULL_CONE, NAT_SYMMETRIC) == NAT_FULL_CONE,
          "reported wins over observe");
    CHECK(punch_admit_hint(PunchAdmit::Relay) == PUNCH_HINT_RELAY, "hint byte");
    CHECK(punch_wait_ms(PUNCH_HINT_RELAY, 6000) == 1500, "relay wait 1.5s");
    CHECK(punch_wait_ms(PUNCH_HINT_ICE, 6000) == 6000, "ice keeps 6s");
    CHECK(punch_wait_ms(PUNCH_HINT_NONE, 6000) == 6000, "old server keeps 6s");
    CHECK(punch_wait_ms(PUNCH_HINT_NEED, 6000) == 6000, "need keeps full timeout");
    CHECK(compose_connect_hint(PunchAdmit::Ice, true, false) == PUNCH_HINT_NEED,
          "need overlays ice");
    CHECK(compose_connect_hint(PunchAdmit::Relay, true, true) == PUNCH_HINT_RELAY,
          "EDM×EDM stays relay even if need_p2p");
    CHECK(compose_connect_hint(PunchAdmit::Ice, false, false) == PUNCH_HINT_ICE,
          "no need keeps ice");
    CHECK(extinfo_has_need_p2p("np=1"), "np=1 parsed");
    CHECK(!extinfo_has_need_p2p("np=0"), "np=0 is not need");
    CHECK(std::string(punch_hint_str(PUNCH_HINT_NEED)) == "need", "hint str");

    LocalListeners loc;
    loc.add_any(18832);
    loc.add_ip("10.0.0.1", 18832);
    CHECK(loc.is_local(addr("127.0.0.1", 18832)), "bind-any skips loopback self");
    CHECK(loc.is_local(addr("10.0.0.1", 18832)), "wan ip+port is self");
    CHECK(!loc.is_local(addr("10.0.0.2", 18832)), "other host same port not self");
    CHECK(!loc.is_local(addr("10.0.0.1", 18833)), "other port not self");

    if (g_fail) {
        printf("punch_admit_test: %d FAIL\n", g_fail);
        return 1;
    }
    printf("punch_admit_test: all ok\n");
    return 0;
}
