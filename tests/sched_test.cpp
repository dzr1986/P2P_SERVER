// sched_test.cpp：P6 区域调度单测（就近排序 + 权重加成）
#include "common/RegionSched.h"
#include "common/Uid.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace p2p;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    CHECK(region_affinity('A', 'A', 'C') == 0, "prefer match = 0");
    CHECK(region_affinity('C', 'A', 'C') == 1, "local match = 1");
    CHECK(region_affinity(0, 'A', 'C') == 2, "unlabeled = 2");
    CHECK(region_affinity('B', 'A', 'C') == 3, "other region = 3");

    CHECK(region_weight_boost('A', 'A', 'C') == 3.0, "prefer boost x3");
    CHECK(region_weight_boost('C', 'A', 'C') == 1.5, "local boost x1.5");
    CHECK(region_weight_boost('B', 'A', 'C') == 1.0, "other no boost");

    std::unordered_map<std::string, char> nat = {
        {"10.0.0.1", 'A'},
        {"10.0.0.2", 'B'},
        {"127.0.0.1", 'C'},
    };
    std::vector<std::string> ips = {"10.0.0.2", "127.0.0.1", "10.0.0.1"};
    sort_ips_by_region(ips, nat, 'A', 'C');
    CHECK(ips.size() == 3 && ips[0] == "10.0.0.1", "prefer A first");
    CHECK(ips[1] == "127.0.0.1", "local C second");
    CHECK(ips[2] == "10.0.0.2", "other B last");

    ips = {"10.0.0.2", "127.0.0.1", "10.0.0.1"};
    sort_ips_by_region(ips, nat, 'C', 'C');
    CHECK(ips[0] == "127.0.0.1", "prefer C first");
    // 其余同亲和度保持稳定相对序：10.0.0.2 在 10.0.0.1 之前
    CHECK(ips[1] == "10.0.0.2" && ips[2] == "10.0.0.1", "stable order among others");

    char uid[UID_LEN + 1];
    CHECK(uid_generate("CAMA", 'A', uid) == 0 && uid_region(uid) == 'A',
          "generated UID region A");

    if (g_fail == 0) { printf("sched tests PASS\n"); return 0; }
    printf("sched tests FAIL (%d)\n", g_fail);
    return 1;
}
