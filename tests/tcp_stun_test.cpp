#include "core/socket/Net.h"
#include "core/packet/StunBind.h"
#include "core/connectivity/hole_punch/TcpPunch.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    uint8_t req[20];
    CHECK(stun_write_binding_request(req, sizeof(req)) == 20, "write binding request");
    CHECK(stun_is_binding_request(req, sizeof(req)), "request recognized");

    TcpFd listen;
    CHECK(listen.open() && listen.set_reuse() && listen.bind_loopback(0) &&
          listen.listen(2), "local tcp stun listen");
    const uint16_t port = listen.local_port();
    CHECK(port != 0, "listen port");

    std::thread srv([&] {
        TcpFd c = listen.accept_one();
        if (!c.valid()) return;
        uint8_t in[20];
        if (c.recv_some(in, sizeof(in)) < 20) return;
        sockaddr_in peer{};
        socklen_t sl = sizeof(peer);
        getpeername(c.fd(), reinterpret_cast<sockaddr*>(&peer), &sl);
        uint8_t rsp[32];
        if (stun_write_binding_success(rsp, sizeof(rsp), in, 20, peer))
            c.send_all(rsp, 32);
    });

    TcpFd cli;
    CHECK(cli.open() && cli.connect_to("127.0.0.1", port), "client connect");
    CHECK(cli.send_all(req, 20) == 20, "send request");
    uint8_t rsp[32];
    CHECK(cli.recv_some(rsp, sizeof(rsp)) == 32, "recv success");
    uint32_t addr = 0;
    uint16_t mport = 0;
    CHECK(stun_read_xor_mapped(rsp, 32, &addr, &mport), "parse mapped");
    char ip[16] = {};
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    CHECK(strcmp(ip, "127.0.0.1") == 0, "mapped ip loopback");
    CHECK(ntohs(mport) != 0, "mapped port nonzero");

    srv.join();
    return g_fail ? 1 : 0;
}
