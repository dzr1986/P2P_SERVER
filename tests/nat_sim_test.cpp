#include "common/NatSim.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    CHECK(nat_sim_stun_punch(NAT_MAP_EIM, NAT_FLT_NONE, NAT_MAP_EIM, NAT_FLT_NONE),
          "EIM/none × EIM/none 全锥直连");
    CHECK(nat_sim_stun_punch(NAT_MAP_EIM, NAT_FLT_PORT, NAT_MAP_EIM, NAT_FLT_PORT),
          "EIM/port × EIM/port 同时打洞");
    CHECK(nat_sim_stun_punch(NAT_MAP_EIM, NAT_FLT_ADDR, NAT_MAP_EIM, NAT_FLT_PORT),
          "EIM/addr × EIM/port");
    CHECK(!nat_sim_stun_punch(NAT_MAP_EIM, NAT_FLT_PORT, NAT_MAP_EDM, NAT_FLT_PORT),
          "EIM × EDM 裸 STUN 口失败");
    CHECK(!nat_sim_stun_punch(NAT_MAP_EDM, NAT_FLT_PORT, NAT_MAP_EDM, NAT_FLT_PORT),
          "EDM × EDM 必须中继");

    CHECK(nat_sim_cone_pair(NAT_MAP_EIM, NAT_MAP_EIM), "cone pair EIM×EIM");
    CHECK(!nat_sim_cone_pair(NAT_MAP_EIM, NAT_MAP_EDM), "EIM×EDM not cone pair");
    CHECK(nat_sim_stun_punch(NAT_MAP_EIM, NAT_FLT_PORT, NAT_MAP_EIM, NAT_FLT_PORT) ==
              nat_sim_cone_pair(NAT_MAP_EIM, NAT_MAP_EIM),
          "sim 直连 ↔ 锥×锥");
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EDM, 0, 2, false) == PunchStrategy::Predict,
          "EIM×EDM+step → predict");
    CHECK(punch_strategy(NAT_MAP_EDM, NAT_MAP_EDM, 0, 0, false) == PunchStrategy::Relay,
          "EDM×EDM → relay");
    CHECK(punch_strategy(NAT_MAP_EDM, NAT_MAP_EDM, 0, 0, true) == PunchStrategy::Birthday,
          "EDM×EDM+birthday → birthday");

    // EIM 两次目的同一映射口
    NatSimBox box;
    box.mapping = NAT_MAP_EIM;
    uint16_t p1 = box.outbound(1, 9, 0x11111111u, 80);
    uint16_t p2 = box.outbound(1, 9, 0x22222222u, 443);
    CHECK(p1 == p2 && p1 != 0, "EIM 不同目的共用映射口");
    box = NatSimBox{};
    box.mapping = NAT_MAP_EDM;
    box.step = 2;
    uint16_t e1 = box.outbound(1, 9, 0x11111111u, 80);
    uint16_t e2 = box.outbound(1, 9, 0x22222222u, 443);
    CHECK(e1 != e2 && (int)e2 - (int)e1 == 2, "EDM 每目的换口且步长=2");

    return g_fail ? 1 : 0;
}
