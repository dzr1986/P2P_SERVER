#include "core/packet/IceSdp.h"

#include <cstdio>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    const std::string sdp1 =
        "v=0\r\n"
        "a=ice-ufrag:AbCd\r\n"
        "a=ice-pwd:secret1\r\n"
        "a=candidate:1 1 UDP 2130706431 127.0.0.1 9 typ host\r\n";
    const std::string sdp2 =
        "v=0\n"
        "a=ice-ufrag:XyZ9\n"
        "a=ice-pwd:secret2\n";
    const std::string sdp1b =
        "v=0\n"
        "a=ice-ufrag:AbCd\n"
        "a=ice-pwd:secret1\n"
        "a=candidate:2 1 UDP 1694498815 1.2.3.4 9 typ srflx\n";

    CHECK(ice_sdp_ufrag(sdp1) == "AbCd", "ufrag from CRLF SDP");
    CHECK(ice_sdp_pwd(sdp1) == "secret1", "pwd from CRLF SDP");
    CHECK(ice_sdp_ufrag(sdp2) == "XyZ9", "ufrag from LF SDP");
    CHECK(ice_sdp_ufrag("no ufrag here") == "", "missing ufrag");
    CHECK(ice_sdp_ufrag("") == "", "empty sdp");
    CHECK(!ice_sdp_is_restart("", sdp1), "empty old is not restart");
    CHECK(!ice_sdp_is_restart(sdp1, sdp1b), "same ufrag trickle is not restart");
    CHECK(ice_sdp_is_restart(sdp1, sdp2), "different ufrag is restart");
    CHECK(!ice_sdp_is_restart(sdp1, "a=candidate:1"), "no ufrag on new");

    if (g_fail) {
        printf("ice_sdp_test FAIL=%d\n", g_fail);
        return 1;
    }
    printf("ice_sdp_test PASS\n");
    return 0;
}
