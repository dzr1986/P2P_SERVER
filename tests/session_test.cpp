// session_test.cpp：Session 可靠传输 + 流媒体特性单测
//   - 无丢包链路：按序全量交付，RTT/RTO 收敛
//   - 有丢包链路：重传（超时/快速重传）保证不丢不乱序
//   - 拥塞窗口：断链时发送被 cwnd 限流
//   - 不可靠通道 FEC：单帧丢失免重传恢复 + 丢包统计
#include "client/sdk/session/Session.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace p2p;

namespace {

int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            printf("  ok: %s\n", msg);                      \
        } else {                                            \
            printf("  FAIL: %s\n", msg);                    \
            g_fail++;                                       \
        }                                                   \
    } while (0)

bool is_unreliable_data(const uint8_t* f, size_t n) {
    return n >= 14 && f[3] == TT_DATA && (f[7] & TF_RELIABLE) == 0;
}
bool is_reliable_data(const uint8_t* f, size_t n) {
    return n >= 14 && f[3] == TT_DATA && (f[7] & TF_RELIABLE) != 0;
}

// 驱动两端 tick 直到条件满足或超时
template <typename Cond>
bool pump(Session& a, Session& b, Cond cond, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (cond()) return true;
        usleep(10 * 1000);
        waited += 10;
        uint64_t now = plat_now_ms();
        a.tick(now);
        b.tick(now);
    }
    return cond();
}

} // namespace

int main() {
    // ---------------------------------------------------------------- 1. 无丢包可靠传输
    {
        Session a(1), b(1);
        std::vector<std::string> rx;
        a.on_tx = [&](const uint8_t* f, size_t n) { b.on_frame(f, n); };
        b.on_tx = [&](const uint8_t* f, size_t n) { a.on_frame(f, n); };
        b.on_data = [&](uint8_t, const uint8_t* d, size_t n) {
            rx.emplace_back((const char*)d, n);
        };
        printf("== [1] reliable, lossless ==\n");
        int sent = 0;
        for (int i = 0; i < 200; i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "m%04d", i);
            // 无丢包时 ACK 同步回流，窗口即时清空，不应出现窗口满
            if (a.send(1, msg, strlen(msg), true) == 0) sent++;
        }
        CHECK(sent == 200, "200 msgs accepted");
        CHECK(rx.size() == 200, "200 msgs delivered");
        bool ordered = true;
        for (int i = 0; i < (int)rx.size(); i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "m%04d", i);
            if (rx[i] != msg) { ordered = false; break; }
        }
        CHECK(ordered, "in-order delivery");
        auto st = a.stats();
        CHECK(st.srtt_ms >= 1, "srtt sampled");
        CHECK(st.rto_ms >= 100 && st.rto_ms <= 3000, "rto in [100,3000]");
        CHECK(st.retrans == 0 && st.fast_retrans == 0, "no retransmission on lossless link");
        CHECK(st.cwnd > 4, "cwnd grew beyond initial");
    }

    // ---------------------------------------------------------------- 2. 丢包可靠传输
    {
        Session a(2), b(2);
        std::vector<std::string> rx;
        int data_n = 0;
        // 每 7 个可靠 DATA 帧丢 1 个（ACK 不丢），触发快速重传/超时重传
        a.on_tx = [&](const uint8_t* f, size_t n) {
            if (is_reliable_data(f, n) && (++data_n % 7) == 0) return;
            b.on_frame(f, n);
        };
        b.on_tx = [&](const uint8_t* f, size_t n) { a.on_frame(f, n); };
        b.on_data = [&](uint8_t, const uint8_t* d, size_t n) {
            rx.emplace_back((const char*)d, n);
        };
        printf("== [2] reliable, lossy (1/7 data drop) ==\n");
        int queued = 0;
        for (int i = 0; i < 100;) {
            char msg[32];
            snprintf(msg, sizeof(msg), "m%04d", i);
            int r = a.send(1, msg, strlen(msg), true);
            if (r == 0) { i++; queued++; continue; }
            // 窗口满：驱动重传/收包后继续
            uint64_t now = plat_now_ms();
            a.tick(now); b.tick(now);
            usleep(5 * 1000);
        }
        bool all = pump(a, b, [&] { return rx.size() >= 100; }, 15000);
        CHECK(all && queued == 100, "100 msgs delivered over lossy link");
        bool ordered = true;
        for (int i = 0; i < (int)rx.size(); i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "m%04d", i);
            if (rx[i] != msg) { ordered = false; break; }
        }
        CHECK(ordered, "in-order delivery under loss");
        auto st = a.stats();
        CHECK(st.retrans + st.fast_retrans > 0, "retransmission happened");
        printf("  info: srtt=%ums rto=%ums cwnd=%u retrans=%llu fast=%llu\n",
               st.srtt_ms, st.rto_ms, st.cwnd,
               (unsigned long long)st.retrans, (unsigned long long)st.fast_retrans);
    }

    // ---------------------------------------------------------------- 3. 拥塞窗口限流
    {
        Session a(3);
        a.on_tx = [](const uint8_t*, size_t) {};   // 黑洞链路：无 ACK
        printf("== [3] congestion window gating ==\n");
        int ok = 0, blocked = 0;
        for (int i = 0; i < 10; i++) {
            if (a.send(1, "x", 1, true) == 0) ok++;
            else blocked++;
        }
        CHECK(ok == 4 && blocked == 6, "initial cwnd=4 gates the 5th send");
    }

    // ---------------------------------------------------------------- 4. 不可靠通道 FEC
    {
        Session a(4), b(4);
        std::vector<std::string> rx;
        int unrel_n = 0;
        // 丢第 2 个不可靠 DATA 帧，FEC 校验帧照常送达
        a.on_tx = [&](const uint8_t* f, size_t n) {
            if (is_unreliable_data(f, n) && ++unrel_n == 2) return;
            b.on_frame(f, n);
        };
        b.on_tx = [&](const uint8_t* f, size_t n) { a.on_frame(f, n); };
        b.on_data = [&](uint8_t, const uint8_t* d, size_t n) {
            rx.emplace_back((const char*)d, n);
        };
        printf("== [4] unreliable channel XOR FEC ==\n");
        const char* frames[4] = {"video-frame-0", "video-frame-11", "video-2", "video-frame-333"};
        for (int i = 0; i < 4; i++) a.send(2, frames[i], strlen(frames[i]), false);
        // 满组即出 FEC 帧，恢复应同步完成
        bool got4 = pump(a, b, [&] { return rx.size() >= 4; }, 2000);
        CHECK(got4, "4 frames delivered (1 recovered)");
        bool has_lost = false;
        for (auto& s : rx) if (s == frames[1]) has_lost = true;
        CHECK(has_lost, "lost frame recovered by FEC");
        auto sa = a.stats();
        auto sb = b.stats();
        CHECK(sa.fec_sent >= 1, "FEC parity frame sent");
        CHECK(sb.fec_recovered == 1, "FEC recovered exactly 1 frame");
        CHECK(sb.rx_lost == 0, "loss estimate refunded after recovery");
    }

    // ---------------------------------------------------------------- 5. 部分组 FEC 冲刷
    {
        Session a(5), b(5);
        std::vector<std::string> rx;
        int unrel_n = 0;
        a.on_tx = [&](const uint8_t* f, size_t n) {
            if (is_unreliable_data(f, n) && ++unrel_n == 1) return;   // 丢组内第 1 帧
            b.on_frame(f, n);
        };
        b.on_tx = [&](const uint8_t* f, size_t n) { a.on_frame(f, n); };
        b.on_data = [&](uint8_t, const uint8_t* d, size_t n) {
            rx.emplace_back((const char*)d, n);
        };
        printf("== [5] partial FEC group flush (low-rate stream) ==\n");
        a.send(2, "audio-0", 7, false);
        a.send(2, "audio-1", 7, false);
        // 仅 2 帧不满组：>=100ms 后 tick 冲刷部分组 FEC
        bool got2 = pump(a, b, [&] { return rx.size() >= 2; }, 2000);
        CHECK(got2, "partial group recovered after flush");
        CHECK(b.stats().fec_recovered == 1, "partial-group FEC recovery counted");
    }

    if (g_fail == 0) {
        printf("session tests PASS\n");
        return 0;
    }
    printf("session tests FAIL (%d)\n", g_fail);
    return 1;
}
