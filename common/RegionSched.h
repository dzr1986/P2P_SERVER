#ifndef P2P_COMMON_REGION_SCHED_H
#define P2P_COMMON_REGION_SCHED_H

// P6：按 UID REGION / 本节点区域对 NAT/Proxy 入口就近排序与加权。
// 亲和度越小越优先：0=请求方 REGION 命中，1=本节点 REGION，2=未标注，3=其它区域。

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace p2p {

inline int region_affinity(char server_region, char prefer, char local) {
    if (prefer && server_region == prefer) return 0;
    if (local && server_region == local) return 1;
    if (server_region == 0) return 2;
    return 3;
}

inline char region_of(const std::string& ip,
                      const std::unordered_map<std::string, char>& map,
                      char fallback = 0) {
    auto it = map.find(ip);
    return it != map.end() ? it->second : fallback;
}

inline void sort_ips_by_region(std::vector<std::string>& ips,
                               const std::unordered_map<std::string, char>& ip_region,
                               char prefer, char local) {
    std::stable_sort(ips.begin(), ips.end(),
        [&](const std::string& a, const std::string& b) {
            const int sa = region_affinity(region_of(a, ip_region, local), prefer, local);
            const int sb = region_affinity(region_of(b, ip_region, local), prefer, local);
            return sa < sb;
        });
}

// 代理负载权重叠加区域亲和：同区 ×3，本节点区 ×1.5
inline double region_weight_boost(char server_region, char prefer, char local) {
    if (prefer && server_region == prefer) return 3.0;
    if (local && server_region == local) return 1.5;
    return 1.0;
}

} // namespace p2p

#endif // P2P_COMMON_REGION_SCHED_H
