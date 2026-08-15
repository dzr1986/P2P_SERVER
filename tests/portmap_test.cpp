#include "core/connectivity/hole_punch/PortMap.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    uint8_t req[12];
    natpmp_write_map_req(req, 12345, 12345, 3600);
    CHECK(req[0] == 0 && req[1] == 1, "nat-pmp req version/op");
    uint16_t ip = 0;
    memcpy(&ip, req + 4, 2);
    CHECK(ntohs(ip) == 12345, "nat-pmp req internal port");

    uint8_t rsp[16]{};
    rsp[0] = 0;
    rsp[1] = (uint8_t)(1 | 0x80);
    uint16_t iport = htons(12345), eport = htons(23456);
    uint32_t life = htonl(1800);
    memcpy(rsp + 8, &iport, 2);
    memcpy(rsp + 10, &eport, 2);
    memcpy(rsp + 12, &life, 4);
    PortMapResult r;
    CHECK(natpmp_read_map_rsp(rsp, sizeof(rsp), r), "nat-pmp rsp parse");
    CHECK(r.ok && r.external_port == 23456 && r.lifetime_sec == 1800,
          "nat-pmp rsp fields");
    CHECK(strcmp(r.backend, "nat-pmp") == 0, "nat-pmp backend tag");

    const std::string xml =
        "<root><device><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1"
        "</serviceType><controlURL>/upnp/control/wanip</controlURL></service>"
        "</serviceList></device></root>";
    std::string ctrl;
    CHECK(upnp_pick_control_url(xml, "http://192.168.1.1:5000/root.xml", ctrl),
          "upnp pick controlURL");
    CHECK(ctrl == "http://192.168.1.1:5000/upnp/control/wanip",
          "upnp resolve relative controlURL");

    std::vector<uint16_t> ports;
    portmap_host_ports_from_sdp(
        "a=candidate:1 1 UDP 2130706431 10.0.0.8 54321 typ host\r\n"
        "a=candidate:2 1 UDP 1694498815 1.2.3.4 9 typ srflx\r\n"
        "a=candidate:3 1 UDP 2130706431 127.0.0.1 11111 typ host\r\n",
        ports);
    CHECK(ports.size() == 1 && ports[0] == 54321, "sdp host port skips loopback/srflx");

    // 假 NAT-PMP：本机 UDP 应答
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(srv >= 0, "mock pmp socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "mock pmp bind");
    socklen_t sl = sizeof(addr);
    getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &sl);
    const uint16_t pmp_port = ntohs(addr.sin_port);
    std::thread th([&] {
        uint8_t in[32];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n = ::recvfrom(srv, in, sizeof(in), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 12) return;
        uint8_t out[16]{};
        out[0] = 0;
        out[1] = (uint8_t)(1 | 0x80);
        memcpy(out + 8, in + 4, 2);
        uint16_t ext = htons(40000);
        memcpy(out + 10, &ext, 2);
        uint32_t lf = htonl(600);
        memcpy(out + 12, &lf, 4);
        ::sendto(srv, out, sizeof(out), 0, reinterpret_cast<sockaddr*>(&from), fl);
    });
    PortMapResult live;
    bool ok = portmap_natpmp_ex(htonl(INADDR_LOOPBACK), pmp_port, 2222, 600, 800, live);
    th.join();
    ::close(srv);
    CHECK(ok && live.external_port == 40000, "nat-pmp against mock server");

    uint32_t gw = 0;
    (void)portmap_default_gateway(&gw); // 有无网关都不算失败
    CHECK(true, "default gateway probe (optional)");

    CHECK(portmap_renew_delay_ms(300) == 200000, "renew 300s → 200s");
    CHECK(portmap_renew_delay_ms(3600) == 2400000, "renew 3600s → 2400s");
    CHECK(portmap_renew_delay_ms(0) == 200000, "renew 0 → default 300s");
    CHECK(portmap_renew_delay_ms(3) == 2000, "renew 3s → 2s");

    if (g_fail) {
        printf("portmap_test FAIL=%d\n", g_fail);
        return 1;
    }
    printf("portmap_test PASS\n");
    return 0;
}
