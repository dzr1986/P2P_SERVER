#include "common/PathSelect.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    PathFacts f{};
    CHECK(select_send_path(f) == PathKind::None, "empty facts → none");

    f.ice_nominated = true;
    f.juice_direct = true;
    f.tcp_punch_ok = true;
    f.have_direct_udp = true;
    f.direct_proven = true;
    f.relay_ready = true;
    f.derp_ready = true;
    f.udp_relay_ready = true;
    CHECK(select_send_path(f) == PathKind::IceNominated, "nominated beats all");
    CHECK(path_kind_str(PathKind::IceNominated)[0] == 'i', "ice name");

    f.ice_nominated = false;
    f.juice_prev = true;
    CHECK(select_send_path(f) == PathKind::JuicePrev, "restart prev beats juice");

    f.juice_prev = false;
    CHECK(select_send_path(f) == PathKind::JuiceDirect, "juice+direct_ok");

    f.juice_direct = false;
    CHECK(select_send_path(f) == PathKind::TcpPunch, "tcp punch before udp");

    f.tcp_punch_ok = false;
    CHECK(select_send_path(f) == PathKind::UdpDirect, "proven udp direct");

    f.have_direct_udp = false;
    f.direct_proven = false;
    CHECK(select_send_path(f) == PathKind::DerpTcp, "derp before udp relay");

    f.derp_ready = false;
    CHECK(select_send_path(f) == PathKind::UdpRelay, "udp relay last");

    // 打洞窗口：中继未就绪但已有公网口 → 先发 UDP，避免空窗
    PathFacts win{};
    win.have_direct_udp = true;
    win.relay_ready = false;
    CHECK(select_send_path(win) == PathKind::UdpDirect, "punch window uses udp");

    // 中继已就绪、直连未证实 → 不抢 UDP（等 ICE/TCP 或走中继）
    PathFacts unproven{};
    unproven.have_direct_udp = true;
    unproven.direct_proven = false;
    unproven.relay_ready = true;
    unproven.udp_relay_ready = true;
    CHECK(select_send_path(unproven) == PathKind::UdpRelay,
          "unproven + relay_ready skips udp");

    // force_relay：跳过全部直连族（测试 [2]/[11]）
    PathFacts fr = f;
    fr.force_relay = true;
    fr.ice_nominated = true;
    fr.tcp_punch_ok = true;
    fr.have_direct_udp = true;
    fr.direct_proven = true;
    fr.relay_ready = true;
    fr.derp_ready = true;
    fr.udp_relay_ready = true;
    CHECK(select_send_path(fr) == PathKind::DerpTcp, "force_relay skips direct");

    PathFacts fr_udp = fr;
    fr_udp.derp_ready = false;
    CHECK(select_send_path(fr_udp) == PathKind::UdpRelay, "force_relay udp relay");

    PathKind buf[8];
    PathFacts all{};
    all.ice_nominated = true;
    all.tcp_punch_ok = true;
    all.have_direct_udp = true;
    all.direct_proven = true;
    all.relay_ready = true;
    all.derp_ready = true;
    all.udp_relay_ready = true;
    int n = fill_send_paths(all, buf, 8);
    CHECK(n == 5, "fill: ice+tcp+udp+derp+relay");
    CHECK(buf[0] == PathKind::IceNominated && buf[1] == PathKind::TcpPunch &&
              buf[2] == PathKind::UdpDirect && buf[3] == PathKind::DerpTcp &&
              buf[4] == PathKind::UdpRelay,
          "fill order");

    if (g_fail) {
        printf("path_select_test: %d FAIL\n", g_fail);
        return 1;
    }
    printf("path_select_test: all ok\n");
    return 0;
}
