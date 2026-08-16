// punch_policy_test：对齐 EasyTier hole_punch/policy.rs
#include "core/connectivity/hole_punch/PunchPolicy.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    PunchBackOff bo({10, 20});
    CHECK(bo.next() == 10, "backoff 10");
    CHECK(bo.next() == 20, "backoff 20");
    CHECK(bo.next() == 20, "backoff saturates");
    bo.rollback();
    CHECK(bo.next() == 10, "rollback then 10");
    bo.reset();
    CHECK(bo.next() == 10, "reset to first");

    PunchBackOff exp = PunchBackOff::exp(1000, 30000);
    CHECK(exp.next() == 1000, "exp 1s");
    CHECK(exp.next() == 2000, "exp 2s");
    CHECK(exp.next() == 4000, "exp 4s");

    PunchPeerFlag none{};
    PunchPeerFlag need{};
    need.need_p2p = true;
    PunchPeerFlag disable{};
    disable.disable_p2p = true;
    PunchPeerFlag pub{};
    pub.is_public_server = true;
    PunchPeerFlag disable_need{};
    disable_need.disable_p2p = true;
    disable_need.need_p2p = true;

    CHECK(should_try_p2p_with_peer(&none, false, false, false), "normal try");
    CHECK(should_try_p2p_with_peer(nullptr, false, false, false), "no flag try");
    CHECK(!should_try_p2p_with_peer(nullptr, false, true, false), "no flag + local disable");
    CHECK(!should_try_p2p_with_peer(&none, false, true, false), "local disable");
    CHECK(should_try_p2p_with_peer(&need, false, true, false), "peer need overrides local disable");
    CHECK(!should_try_p2p_with_peer(&disable, false, false, false), "peer disable");
    CHECK(should_try_p2p_with_peer(&disable, false, false, true), "local need overrides peer disable");
    CHECK(should_try_p2p_with_peer(&disable_need, false, true, true), "both need");
    CHECK(!should_try_p2p_with_peer(&disable_need, false, true, false), "peer disable+need, local no need");
    CHECK(!should_try_p2p_with_peer(&pub, false, false, false), "public server blocked");
    CHECK(should_try_p2p_with_peer(&pub, true, false, false), "public server allowed");

    CHECK(should_background_p2p_with_peer(&none, false, false, false, false),
          "not lazy → background");
    CHECK(!should_background_p2p_with_peer(&none, false, true, false, false),
          "lazy + peer no need → no background");
    CHECK(should_background_p2p_with_peer(&need, false, true, false, false),
          "lazy + peer need → background");
    CHECK(!should_background_p2p_with_peer(&pub, false, false, false, false),
          "public server no background");
    CHECK(should_background_p2p_with_peer(&pub, true, false, false, false),
          "public allowed → background");

    PunchGate g;
    CHECK(should_try_p2p(g), "iot default try");
    CHECK(should_background_p2p(g), "iot default background");
    g.force_relay = true;
    CHECK(!should_try_p2p(g), "force_relay hard stop try");
    CHECK(!should_background_p2p(g), "force_relay hard stop background");
    g.peer_need_p2p = true;
    CHECK(!should_try_p2p(g), "force_relay ignores peer need");
    g = PunchGate{};
    g.lazy_p2p = true;
    CHECK(!should_background_p2p(g), "lazy no traffic → no background");
    g.want_direct = true;
    CHECK(should_background_p2p(g), "lazy + send → background");
    g.want_direct = false;
    g.peer_need_p2p = true;
    CHECK(should_background_p2p(g), "lazy + peer punch → background");

    if (g_fail) {
        printf("punch_policy_test: %d FAIL\n", g_fail);
        return 1;
    }
    printf("punch_policy_test: all ok\n");
    return 0;
}
