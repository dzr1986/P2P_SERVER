#include "core/tunnel/TwccEstimate.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    DelayKalman kf;
    CHECK(kf.update(0) < 1 && kf.update(0) > -1, "kalman stays near 0");
    double x = 0;
    for (int i = 0; i < 20; i++) x = kf.update(20);
    CHECK(x > 10, "kalman tracks +20ms queueing");

    TwccEstimator est;
    CHECK(est.suggested_kbps() == ABR_BASE_KBPS, "bootstrap base");
    // 发送间隔 20ms，到达间隔 40ms → 排队增加 → overuse → MD
    for (uint16_t i = 0; i < 8; i++) {
        est.on_send(i, 1000 + i * 20);
        est.on_feedback(i, 2000 + i * 40);
    }
    CHECK(est.overuse == 1, "growing delay marks overuse");
    CHECK(est.suggested_kbps() < ABR_BASE_KBPS, "overuse multiplies down");
    CHECK(est.fb_count == 8, "all feedback consumed");

    TwccEstimator under;
    for (uint16_t i = 0; i < 8; i++) {
        under.on_send(i, 1000 + i * 40);
        under.on_feedback(i, 2000 + i * 20);  // 到达更快 → underuse
    }
    CHECK(under.overuse == -1, "shrinking delay marks underuse");
    CHECK(under.suggested_kbps() > ABR_BASE_KBPS, "underuse additive increase");

    TwccReceiver rx;
    rx.on_recv(1, 100);
    rx.on_recv(2, 120);
    CHECK(!rx.should_flush(130), "2 samples wait");
    rx.on_recv(3, 140);
    rx.on_recv(4, 160);
    CHECK(rx.should_flush(160), "4 samples flush");
    uint8_t buf[64];
    size_t n = rx.write(buf, sizeof(buf));
    CHECK(n == 1 + 4 * 6, "feedback wire size");
    CHECK(buf[0] == 4, "feedback count");
    CHECK(!rx.should_flush(200), "flushed empty");

    TwccEstimator round;
    round.on_send(1, 100);
    round.on_send(2, 120);
    TwccReceiver::parse(buf, n, round);
    CHECK(round.fb_count == 2, "parse applies matching seq");

    // 与 delay-based 混合：TWCC 更低时把目标往下拉
    AbrController ctl;
    int first = ctl.update(40, 8, 16, 0);     // bootstrap → 6000
    int blended = ctl.update(40, 8, 16, 0, 800);
    CHECK(first == 6000, "controller bootstrap underuse");
    CHECK(blended < first, "twcc blend pulls target down");

    if (g_fail) {
        printf("twcc_test FAIL=%d\n", g_fail);
        return 1;
    }
    printf("twcc_test PASS\n");
    return 0;
}
