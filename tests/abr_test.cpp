// abr_test：delay-based 码率建议（学 GCC overuse 分档）
#include "common/AbrEstimate.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    CHECK(abr_suggest_kbps(0, 0, 16, 0) == ABR_BASE_KBPS, "unknown rtt keeps base");
    CHECK(abr_suggest_kbps(40, 8, 16, 0) == 6000, "low delay underuse -> 1080p");
    CHECK(abr_suggest_kbps(250, 20, 16, 0) == 2000, "srtt 250 mild overuse");
    CHECK(abr_suggest_kbps(80, 50, 16, 0) == 2000, "high jitter mild overuse");
    CHECK(abr_suggest_kbps(450, 10, 16, 0) == 800, "srtt 450 hard overuse");
    CHECK(abr_suggest_kbps(40, 8, 16, 5) == 4500, "underuse then loss 3/4");
    CHECK(abr_suggest_kbps(40, 8, 4, 0) == 3000, "cwnd=4 halves 6000");
    CHECK(abr_suggest_kbps(500, 100, 2, 10) >= ABR_MIN_KBPS, "floor at min");
    CHECK(abr_suggest_kbps(10, 1, 64, 0) <= ABR_MAX_KBPS, "cap at max");

    CHECK(abr_aimd_step(0, 6000, false) == 6000, "AIMD bootstrap snaps to target");
    CHECK(abr_aimd_step(4000, 6000, false) == 4200, "AIMD additive +200");
    CHECK(abr_aimd_step(4200, 6000, false) == 4400, "AIMD additive again");
    CHECK(abr_aimd_step(5900, 6000, false) == 6000, "AIMD AI clamps to target");
    int md = abr_aimd_step(4400, 2000, true);
    CHECK(md < 4400 && md > 2000, "AIMD MD + blend toward lower target");
    CHECK(abr_aimd_step(800, 800, false) == 800, "AIMD hold at target");

    AbrController ctl;
    CHECK(ctl.update(0, 0, 16, 0) == ABR_BASE_KBPS, "controller bootstrap base");
    CHECK(ctl.update(40, 8, 16, 0) == ABR_BASE_KBPS + ABR_AI_KBPS, "controller AI underuse");
    CHECK(ctl.update(40, 8, 16, 0) == ABR_BASE_KBPS + 2 * ABR_AI_KBPS, "controller AI again");
    int after_rise = ctl.update(80, 8, 16, 0);
    CHECK(after_rise < ABR_BASE_KBPS + 2 * ABR_AI_KBPS, "controller MD on rising RTT");

    if (g_fail) {
        printf("abr_test FAIL=%d\n", g_fail);
        return 1;
    }
    printf("abr_test PASS\n");
    return 0;
}
