// stun_test.cpp：RFC 8489 Binding Success / XOR-MAPPED-ADDRESS
#include "common/StunBind.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    uint8_t req[20] = {0};
    req[1] = 0x01;                          // Binding Request
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;
    for (int i = 8; i < 20; i++) req[i] = (uint8_t)(0xA0 + i);

    uint8_t built[20];
    CHECK(stun_write_binding_request(built, sizeof(built), req + 8) == 20,
          "write binding request 20B");
    CHECK(stun_is_binding_request(built, sizeof(built)), "written request detected");

    CHECK(stun_is_binding_request(req, sizeof(req)), "binding request detected");
    req[1] = 0x03;
    CHECK(!stun_is_binding_request(req, sizeof(req)), "non-binding rejected");
    req[1] = 0x01;

    sockaddr_in mapped{};
    mapped.sin_family = AF_INET;
    mapped.sin_port = htons(4433);
    inet_pton(AF_INET, "203.0.113.7", &mapped.sin_addr);

    uint8_t rsp[32];
    CHECK(stun_write_binding_success(rsp, sizeof(rsp), req, sizeof(req), mapped) == 32,
          "write success 32B");
    CHECK(memcmp(rsp + 4, req + 4, 16) == 0, "magic+tid echoed");

    uint32_t addr_be = 0;
    uint16_t port_be = 0;
    CHECK(stun_read_xor_mapped(rsp, sizeof(rsp), &addr_be, &port_be), "parse xor-mapped");
    CHECK(ntohs(port_be) == 4433, "mapped port 4433");
    char ip[16] = {0};
    inet_ntop(AF_INET, &addr_be, ip, sizeof(ip));
    CHECK(strcmp(ip, "203.0.113.7") == 0, "mapped ip 203.0.113.7");

    uint8_t junk[20] = {0};
    CHECK(stun_write_binding_success(rsp, sizeof(rsp), junk, sizeof(junk), mapped) == 0,
          "junk request rejected");

    if (g_fail == 0) { printf("stun tests PASS\n"); return 0; }
    printf("stun tests FAIL (%d)\n", g_fail);
    return 1;
}
