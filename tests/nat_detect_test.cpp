#include "client/sdk/transport/NatDetect.h"
#include "common/NatMatrix.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

static void feed_idx(NatDetect& d, uint8_t idx, uint16_t mapped,
                     uint16_t probe = 0) {
    uint8_t buf[sizeof(NatDetectRsp)];
    size_t n = 0;
    NatDetect::pack_rsp(buf, &n, idx, mapped, 18832, 18835, probe);
    d.feed(buf, n);
}

int main() {
    // ---- 全锥：Main 同时收到 idx=0/1 ----
    {
        NatDetect d;
        int sent = 0;
        auto send = [&](const sockaddr_in&, const uint8_t*, size_t) {
            sent++;
            return 0;
        };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 40000, 18836);
        feed_idx(d, 1, 40000, 18836);
        CHECK(d.done(), "full-cone done");
        CHECK(d.nattype() == NAT_FULL_CONE, "full-cone four-type");
        CHECK(d.mapping() == NAT_MAP_EIM, "full-cone mapping=EIM");
        CHECK(d.filter() == NAT_FLT_NONE, "full-cone filter=none");
        CHECK(d.port_step() == 0, "full-cone no nat4e");
        CHECK(sent >= 1, "full-cone sent main req");
    }

    // ---- 端口受限：仅 idx=0，Alt 映射端口相同 ----
    {
        NatDetect d;
        d.set_timeouts(100, 0);
        std::vector<uint16_t> dests;
        auto send = [&](const sockaddr_in& to, const uint8_t*, size_t) {
            dests.push_back(ntohs(to.sin_port));
            return 0;
        };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 41000, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 200);
        CHECK(!d.done(), "port-restricted waits alt");
        feed_idx(d, 0, 41000, 18836);
        CHECK(d.done(), "port-restricted done");
        CHECK(d.nattype() == NAT_PORT_RESTRICTED, "port-restricted four-type");
        CHECK(d.mapping() == NAT_MAP_EIM, "port-restricted mapping=EIM");
        CHECK(d.filter() == NAT_FLT_PORT, "port-restricted filter=port");
        CHECK(d.port_step() == 0, "port-restricted no nat4e");
        CHECK(!dests.empty() && dests.back() == 18835, "alt dest port used");
    }

    // ---- 对称 / EDM：Alt 映射端口不同，无第三口则无 step ----
    {
        NatDetect d;
        d.set_timeouts(100, 0);
        auto send = [&](const sockaddr_in&, const uint8_t*, size_t) { return 0; };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 42000, 0);          // 无 probe_port
        d.tick(send, "127.0.0.1", 18832, 18835, 200);
        feed_idx(d, 1, 42010, 0);
        CHECK(d.done(), "symmetric done without probe");
        CHECK(d.nattype() == NAT_SYMMETRIC, "symmetric four-type");
        CHECK(d.mapping() == NAT_MAP_EDM, "symmetric mapping=EDM");
        CHECK(d.filter() == NAT_FLT_PORT, "symmetric filter=port");
        CHECK(d.port_step() == 0, "symmetric no step without probe");
    }

    // ---- NAT4E：端口 +2 / +2 ----
    {
        NatDetect d;
        d.set_timeouts(50, 0);
        std::vector<uint16_t> dests;
        auto send = [&](const sockaddr_in& to, const uint8_t*, size_t) {
            dests.push_back(ntohs(to.sin_port));
            return 0;
        };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 50000, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 100);
        feed_idx(d, 0, 50002, 18836);
        CHECK(!d.done(), "nat4e enters predict");
        d.tick(send, "127.0.0.1", 18832, 18835, 200);
        feed_idx(d, 2, 50004, 18836);
        CHECK(d.done(), "nat4e done");
        CHECK(d.nattype() == NAT_SYMMETRIC, "nat4e four-type still symmetric");
        CHECK(d.mapping() == NAT_MAP_EDM, "nat4e mapping=EDM");
        CHECK(d.port_step() == 2, "nat4e step=+2");
        bool saw_probe = false;
        for (uint16_t p : dests) if (p == 18836) saw_probe = true;
        CHECK(saw_probe, "nat4e sent to probe port");
    }

    // ---- NAT4E 递减 ----
    {
        NatDetect d;
        d.set_timeouts(50, 0);
        auto send = [&](const sockaddr_in&, const uint8_t*, size_t) { return 0; };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 51000, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 100);
        feed_idx(d, 0, 50999, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 200);
        feed_idx(d, 2, 50998, 18836);
        CHECK(d.port_step() == -1, "nat4e step=-1");
    }

    // ---- 非单调：有第三口但步长不等 → step=0 ----
    {
        NatDetect d;
        d.set_timeouts(50, 0);
        auto send = [&](const sockaddr_in&, const uint8_t*, size_t) { return 0; };
        d.start("127.0.0.1", 18832, send, 0);
        feed_idx(d, 0, 52000, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 100);
        feed_idx(d, 0, 52007, 18836);
        d.tick(send, "127.0.0.1", 18832, 18835, 200);
        feed_idx(d, 2, 52100, 18836);
        CHECK(d.done() && d.mapping() == NAT_MAP_EDM, "random edm done");
        CHECK(d.port_step() == 0, "random edm no step");
    }

    // ---- 旧应答（无 probe_port 字段）仍能解析 ----
    {
        NatDetect d;
        NatDetectRsp old{};
        old.server_index = 0;
        old.mapped_port = htons(43000);
        old.main_port = htons(18832);
        old.alt_port = htons(18835);
        // 截到旧长度（不含 probe_port）
        const size_t old_len = sizeof(NatDetectRsp) - sizeof(uint16_t);
        d.start("127.0.0.1", 18832,
                [](const sockaddr_in&, const uint8_t*, size_t) { return 0; }, 0);
        d.feed(reinterpret_cast<const uint8_t*>(&old), old_len);
        NatDetectRsp old1 = old;
        old1.server_index = 1;
        d.feed(reinterpret_cast<const uint8_t*>(&old1), old_len);
        CHECK(d.done() && d.nattype() == NAT_FULL_CONE, "old rsp without probe_port");
    }

    // ---- NAT 矩阵策略 ----
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EIM, 0, 0) == PunchStrategy::Ice,
          "EIM×EIM → ice");
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EDM, 0, 2) == PunchStrategy::Predict,
          "EIM×EDM+step → predict");
    CHECK(punch_strategy(NAT_MAP_EDM, NAT_MAP_EIM, -1, 0) == PunchStrategy::Predict,
          "EDM+step×EIM → predict");
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EDM, 0, 0) == PunchStrategy::Ice,
          "EIM×EDM no step → ice");
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EDM, 0, 0, true) == PunchStrategy::Birthday,
          "EIM×EDM birthday on → birthday");
    CHECK(punch_strategy(NAT_MAP_EDM, NAT_MAP_EDM, 1, 1) == PunchStrategy::Relay,
          "EDM×EDM → relay");
    CHECK(punch_strategy(NAT_MAP_EDM, NAT_MAP_EDM, 1, 1, true) == PunchStrategy::Birthday,
          "EDM×EDM birthday on → birthday");
    CHECK(strcmp(nat_mapping_str(NAT_MAP_EIM), "EIM") == 0, "mapping str");
    CHECK(strcmp(nat_filter_str(NAT_FLT_PORT), "port") == 0, "filter str");
    CHECK(strcmp(punch_strategy_str(PunchStrategy::Relay), "relay") == 0,
          "strategy str");

    if (g_fail) {
        printf("nat_detect_test FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("nat_detect_test ok\n");
    return 0;
}
