#ifndef P2P_COMMON_PORT_MAP_H
#define P2P_COMMON_PORT_MAP_H

// 家宽主动开孔：UPnP IGD → NAT-PMP / PCP（学 EasyTier）
// 成功则把 EDM 近似成 EIM，零服务器成本。失败是常态，调用方不得当唯一路径。

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace p2p {

struct PortMapResult {
    bool        ok = false;
    const char* backend = "";          // "nat-pmp" | "pcp" | "upnp"
    uint32_t    gateway_nbo = 0;       // 网关 IPv4（网络序）
    uint16_t    internal_port = 0;
    uint16_t    external_port = 0;
    uint32_t    lifetime_sec = 0;
    char        external_ip[16] = {};
};

// /proc/net/route 默认网关；失败返回 false
bool portmap_default_gateway(uint32_t* gw_nbo);

// NAT-PMP（RFC 6886）UDP MAP。pmp_port 默认 5351；测试可指向本机假服务。
bool portmap_natpmp_ex(uint32_t gw_nbo, uint16_t pmp_port, uint16_t int_port,
                       uint32_t lifetime_sec, int timeout_ms, PortMapResult& out);
bool portmap_natpmp(uint16_t int_port, uint32_t lifetime_sec, PortMapResult& out);

// PCP MAP（RFC 6887）version=2，失败则调用方回退 NAT-PMP
bool portmap_pcp_ex(uint32_t gw_nbo, uint16_t pcp_port, uint16_t int_port,
                    uint32_t lifetime_sec, int timeout_ms, PortMapResult& out);

// UPnP IGD：SSDP 发现 + AddPortMapping。timeout 为发现+SOAP 总预算。
bool portmap_upnp(uint16_t int_port, uint32_t lifetime_sec, int timeout_ms,
                  PortMapResult& out);

// 先 IGD，失败再 NAT-PMP，再 PCP
bool portmap_any(uint16_t int_port, uint32_t lifetime_sec, PortMapResult& out);

// ---- 编解码 / 解析（单测用）----
void natpmp_write_map_req(uint8_t out[12], uint16_t int_port, uint16_t sug_ext,
                          uint32_t lifetime_sec);
bool natpmp_read_map_rsp(const uint8_t* p, size_t n, PortMapResult& out);

// 从 IGD description XML 取 WANIP/WANPPP controlURL（相对或绝对）
bool upnp_pick_control_url(const std::string& xml, const std::string& base,
                           std::string& control_url);

// juice / SDP host 候选里抽出 UDP 端口（跳过 127.0.0.1）
void portmap_host_ports_from_sdp(const char* sdp, std::vector<uint16_t>& ports);

// 续约时刻：租约 2/3 处（EasyTier ~300s 租 / 240s 续）。lifetime=0 按 300s。
inline uint32_t portmap_renew_delay_ms(uint32_t lifetime_sec) {
    if (lifetime_sec == 0) lifetime_sec = 300;
    uint32_t sec = lifetime_sec * 2 / 3;
    if (sec < 30 && lifetime_sec > 5) sec = lifetime_sec > 30 ? 30 : (lifetime_sec - 1);
    if (sec < 1) sec = 1;
    return sec * 1000u;
}

} // namespace p2p

#endif // P2P_COMMON_PORT_MAP_H
