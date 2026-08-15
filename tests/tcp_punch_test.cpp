#include "common/TcpPunch.h"
#include "common/NatMatrix.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

static bool wait_ready(int fd, bool write, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = write ? select(fd + 1, nullptr, &fds, nullptr, &tv)
                  : select(fd + 1, &fds, nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(fd, &fds);
}

int main() {
    CHECK(tcp_punch_can_initiate(NAT_MAP_EIM), "EIM can initiate");
    CHECK(tcp_punch_can_initiate(NAT_MAP_ADM), "ADM can initiate");
    CHECK(!tcp_punch_can_initiate(NAT_MAP_EDM), "EDM skip initiate");
    CHECK(!tcp_punch_can_initiate(NAT_MAP_UNKNOWN), "unknown skip initiate");
    CHECK(punch_strategy(NAT_MAP_EIM, NAT_MAP_EIM, 0, 0, false) == PunchStrategy::Ice,
          "EIM×EIM still ICE (TCP is parallel)");

    uint8_t frame[64];
    const char* hello = "hi";
    size_t n = tcp_punch_frame(frame, sizeof(frame), hello, 2);
    CHECK(n == sizeof(MsgHead) + 2, "frame size");
    MsgHead h{};
    memcpy(&h, frame, sizeof(h));
    CHECK(ntohs(h.magic) == NAT_MAGIC && h.msg_id == MSG_TCP_DATA, "frame head");
    CHECK(ntohl(h.length) == 2, "frame payload len");

    // 回环 simultaneous open：两端 listen + 从同一本地口互连
    TcpFd la, lb, ca, cb;
    uint16_t pa = 0, pb = 0;
    CHECK(tcp_punch_listen(la, &pa) && pa != 0, "A listen");
    CHECK(tcp_punch_listen(lb, &pb) && pb != 0 && pb != pa, "B listen");

    int ra = tcp_punch_connect_nb(ca, pa, "127.0.0.1", pb);
    int rb = tcp_punch_connect_nb(cb, pb, "127.0.0.1", pa);
    CHECK(ra >= 0 || rb >= 0, "at least one connect started");

    bool a_ok = (ra == 1);
    bool b_ok = (rb == 1);
    for (int i = 0; i < 40 && (!a_ok || !b_ok); i++) {
        if (!a_ok && ca.valid() && wait_ready(ca.fd(), true, 50)) {
            int d = tcp_punch_connect_done(ca.fd());
            if (d == 1) a_ok = true;
            else if (d < 0) ca.close();
        }
        if (!b_ok && cb.valid() && wait_ready(cb.fd(), true, 50)) {
            int d = tcp_punch_connect_done(cb.fd());
            if (d == 1) b_ok = true;
            else if (d < 0) cb.close();
        }
        if (!a_ok && la.valid() && wait_ready(la.fd(), false, 10)) {
            TcpFd acc = la.accept_one();
            if (acc.valid()) {
                ca = std::move(acc);
                a_ok = true;
            }
        }
        if (!b_ok && lb.valid() && wait_ready(lb.fd(), false, 10)) {
            TcpFd acc = lb.accept_one();
            if (acc.valid()) {
                cb = std::move(acc);
                b_ok = true;
            }
        }
    }
    CHECK(a_ok && b_ok, "simultaneous open or accept both sides");
    CHECK(ca.valid() && cb.valid(), "both sockets valid");

    if (a_ok && b_ok && ca.valid() && cb.valid()) {
        const char ping[] = "P";
        CHECK(ca.send_all(ping, 1) == 1, "A send");
        char rbuf = 0;
        if (wait_ready(cb.fd(), false, 500))
            CHECK(cb.recv_some(&rbuf, 1) == 1 && rbuf == 'P', "B recv");
        else
            CHECK(false, "B recv timeout");
    }

    return g_fail ? 1 : 0;
}
