#include "PortMap.h"
#include "Log.h"
#include "Net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <strings.h>

namespace p2p {

namespace {

bool send_recv_udp(uint32_t dst_nbo, uint16_t port, const void* req, size_t reqn,
                   uint8_t* rsp, size_t rspcap, size_t* outn, int timeout_ms) {
    TcpFd dummy; // 不用；下面直接 socket
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = dst_nbo;
    to.sin_port = htons(port);
    if (::sendto(fd, req, reqn, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
        ::close(fd);
        return false;
    }
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(fd, rsp, rspcap, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    ::close(fd);
    if (n <= 0) return false;
    *outn = (size_t)n;
    return true;
}

std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.find("http://") == 0 || rel.find("https://") == 0) return rel;
    auto pos = base.find("://");
    if (pos == std::string::npos) return rel;
    auto slash = base.find('/', pos + 3);
    std::string origin = (slash == std::string::npos) ? base : base.substr(0, slash);
    if (!rel.empty() && rel[0] == '/') return origin + rel;
    if (slash == std::string::npos) return origin + "/" + rel;
    auto dir = base.rfind('/');
    return base.substr(0, dir + 1) + rel;
}

bool http_get(const std::string& url, std::string& body, int timeout_ms) {
    // 仅 http://host[:port]/path
    if (url.compare(0, 7, "http://") != 0) return false;
    std::string rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    uint16_t port = 80;
    std::string host = hostport;
    auto c = hostport.find(':');
    if (c != std::string::npos) {
        host = hostport.substr(0, c);
        port = (uint16_t)atoi(hostport.substr(c + 1).c_str());
    }
    TcpFd fd;
    if (!fd.open() || !fd.set_timeout_ms(timeout_ms) || !fd.connect_to(host.c_str(), port))
        return false;
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostport +
                      "\r\nConnection: close\r\n\r\n";
    if (fd.send_all(req.data(), req.size()) < 0) return false;
    std::string raw;
    char buf[2048];
    for (;;) {
        ssize_t n = fd.recv_some(buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        if (raw.size() > 64 * 1024) break;
    }
    auto hdr = raw.find("\r\n\r\n");
    if (hdr == std::string::npos) return false;
    body = raw.substr(hdr + 4);
    return raw.find("200") != std::string::npos;
}

bool http_post(const std::string& url, const std::string& soap_action,
               const std::string& xml, std::string& body, int timeout_ms) {
    if (url.compare(0, 7, "http://") != 0) return false;
    std::string rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    uint16_t port = 80;
    std::string host = hostport;
    auto c = hostport.find(':');
    if (c != std::string::npos) {
        host = hostport.substr(0, c);
        port = (uint16_t)atoi(hostport.substr(c + 1).c_str());
    }
    TcpFd fd;
    if (!fd.open() || !fd.set_timeout_ms(timeout_ms) || !fd.connect_to(host.c_str(), port))
        return false;
    std::ostringstream oss;
    oss << "POST " << path << " HTTP/1.0\r\n"
        << "Host: " << hostport << "\r\n"
        << "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        << "SOAPAction: \"" << soap_action << "\"\r\n"
        << "Content-Length: " << xml.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << xml;
    std::string req = oss.str();
    if (fd.send_all(req.data(), req.size()) < 0) return false;
    std::string raw;
    char buf[2048];
    for (;;) {
        ssize_t n = fd.recv_some(buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        if (raw.size() > 64 * 1024) break;
    }
    auto hdr = raw.find("\r\n\r\n");
    if (hdr == std::string::npos) return false;
    body = raw.substr(hdr + 4);
    return raw.find("200") != std::string::npos;
}

} // namespace

bool portmap_default_gateway(uint32_t* gw_nbo) {
    if (!gw_nbo) return false;
    std::ifstream in("/proc/net/route");
    if (!in) return false;
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string iface, dest, gw;
        int flags = 0;
        if (!(iss >> iface >> dest >> gw >> flags)) continue;
        if (dest != "00000000") continue;
        if ((flags & 0x2) == 0) continue; // RTF_GATEWAY
        char* end = nullptr;
        unsigned long v = strtoul(gw.c_str(), &end, 16);
        if (!end || *end) continue;
        *gw_nbo = (uint32_t)v; // 文件里已是网络序字节的 hex
        return *gw_nbo != 0;
    }
    return false;
}

void natpmp_write_map_req(uint8_t out[12], uint16_t int_port, uint16_t sug_ext,
                          uint32_t lifetime_sec) {
    out[0] = 0; // version
    out[1] = 1; // UDP map
    out[2] = 0;
    out[3] = 0;
    uint16_t ip = htons(int_port);
    uint16_t ep = htons(sug_ext);
    uint32_t lt = htonl(lifetime_sec);
    memcpy(out + 4, &ip, 2);
    memcpy(out + 6, &ep, 2);
    memcpy(out + 8, &lt, 4);
}

bool natpmp_read_map_rsp(const uint8_t* p, size_t n, PortMapResult& out) {
    if (!p || n < 16) return false;
    if (p[0] != 0) return false;
    if (p[1] != (uint8_t)(1 | 0x80)) return false;
    uint16_t result = 0, iport = 0, eport = 0;
    uint32_t life = 0;
    memcpy(&result, p + 2, 2);
    memcpy(&iport, p + 8, 2);
    memcpy(&eport, p + 10, 2);
    memcpy(&life, p + 12, 4);
    if (ntohs(result) != 0) return false;
    out.ok = true;
    out.backend = "nat-pmp";
    out.internal_port = ntohs(iport);
    out.external_port = ntohs(eport);
    out.lifetime_sec = ntohl(life);
    return true;
}

bool portmap_natpmp_ex(uint32_t gw_nbo, uint16_t pmp_port, uint16_t int_port,
                       uint32_t lifetime_sec, int timeout_ms, PortMapResult& out) {
    out = {};
    uint8_t req[12];
    natpmp_write_map_req(req, int_port, int_port, lifetime_sec);
    uint8_t rsp[32];
    size_t n = 0;
    if (!send_recv_udp(gw_nbo, pmp_port, req, sizeof(req), rsp, sizeof(rsp), &n,
                       timeout_ms))
        return false;
    if (!natpmp_read_map_rsp(rsp, n, out)) return false;
    out.gateway_nbo = gw_nbo;
    return true;
}

bool portmap_natpmp(uint16_t int_port, uint32_t lifetime_sec, PortMapResult& out) {
    uint32_t gw = 0;
    if (!portmap_default_gateway(&gw)) return false;
    return portmap_natpmp_ex(gw, 5351, int_port, lifetime_sec, 400, out);
}

bool portmap_pcp_ex(uint32_t gw_nbo, uint16_t pcp_port, uint16_t int_port,
                    uint32_t lifetime_sec, int timeout_ms, PortMapResult& out) {
    // RFC 6887 MAP：24B 公共头 + 36B MAP
    out = {};
    uint8_t req[60]{};
    req[0] = 2;          // version
    req[1] = 1;          // MAP
    uint32_t lt = htonl(lifetime_sec);
    memcpy(req + 4, &lt, 4);
    // reserved 4 + client IP 16 置 0（IPv4 映射时网关常忽略）
    // MAP: nonce 12 + proto 1 + reserved 3 + int port 2 + sug ext 2 + ext ip 16
    req[24 + 12] = 17; // UDP
    uint16_t ip = htons(int_port);
    memcpy(req + 24 + 16, &ip, 2);
    memcpy(req + 24 + 18, &ip, 2);
    uint8_t rsp[80];
    size_t n = 0;
    if (!send_recv_udp(gw_nbo, pcp_port, req, sizeof(req), rsp, sizeof(rsp), &n,
                       timeout_ms))
        return false;
    if (n < 60 || rsp[0] != 2 || (rsp[1] & 0x7f) != 1) return false;
    if (rsp[3] != 0) return false; // result code
    uint32_t life = 0;
    uint16_t eport = 0;
    memcpy(&life, rsp + 4, 4);
    memcpy(&eport, rsp + 24 + 18, 2);
    out.ok = true;
    out.backend = "pcp";
    out.gateway_nbo = gw_nbo;
    out.internal_port = int_port;
    out.external_port = ntohs(eport);
    out.lifetime_sec = ntohl(life);
    return out.external_port != 0;
}

bool upnp_pick_control_url(const std::string& xml, const std::string& base,
                           std::string& control_url) {
    const char* keys[] = {"WANIPConnection:1", "WANIPConnection:2",
                          "WANPPPConnection:1", "WANIPConnection"};
    size_t hit = std::string::npos;
    for (const char* k : keys) {
        hit = xml.find(k);
        if (hit != std::string::npos) break;
    }
    if (hit == std::string::npos) return false;
    auto tag = xml.find("<controlURL>", hit);
    if (tag == std::string::npos) {
        // 有的设备 controlURL 在 service 块更前
        auto blk = xml.rfind("<service>", hit);
        if (blk != std::string::npos) tag = xml.find("<controlURL>", blk);
    }
    if (tag == std::string::npos) return false;
    auto start = tag + 12;
    auto end = xml.find("</controlURL>", start);
    if (end == std::string::npos) return false;
    std::string rel = xml.substr(start, end - start);
    control_url = resolve_url(base, rel);
    return !control_url.empty();
}

bool portmap_upnp(uint16_t int_port, uint32_t lifetime_sec, int timeout_ms,
                  PortMapResult& out) {
    out = {};
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = (timeout_ms > 400 ? 400 : timeout_ms) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const char* msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:service:WANIPConnection:1\r\n"
        "\r\n";
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    ::sendto(fd, msearch, strlen(msearch), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));

    char buf[2048];
    std::string location;
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    const char* loc = strcasestr(buf, "LOCATION:");
    if (!loc) loc = strstr(buf, "location:");
    if (!loc) return false;
    loc += 9;
    while (*loc == ' ' || *loc == '\t') loc++;
    const char* e = loc;
    while (*e && *e != '\r' && *e != '\n') e++;
    location.assign(loc, e);
    if (location.empty()) return false;

    std::string desc;
    if (!http_get(location, desc, timeout_ms)) return false;
    std::string ctrl;
    if (!upnp_pick_control_url(desc, location, ctrl)) return false;

    char local_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    // 用已连接的假地址不够；SOAP 里 NewInternalClient 用网卡地址近似：对端 from
    inet_ntop(AF_INET, &from.sin_addr, local_ip, sizeof(local_ip));
    // 更好：取本机连网关的源地址
    {
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            sockaddr_in gw{};
            gw.sin_family = AF_INET;
            gw.sin_addr = from.sin_addr;
            gw.sin_port = htons(80);
            if (::connect(s, reinterpret_cast<sockaddr*>(&gw), sizeof(gw)) == 0) {
                sockaddr_in me{};
                socklen_t sl = sizeof(me);
                if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &sl) == 0)
                    inet_ntop(AF_INET, &me.sin_addr, local_ip, sizeof(local_ip));
            }
            ::close(s);
        }
    }

    std::ostringstream soap;
    soap << "<?xml version=\"1.0\"?>"
         << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
         << "<s:Body>"
         << "<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
         << "<NewRemoteHost></NewRemoteHost>"
         << "<NewExternalPort>" << int_port << "</NewExternalPort>"
         << "<NewProtocol>UDP</NewProtocol>"
         << "<NewInternalPort>" << int_port << "</NewInternalPort>"
         << "<NewInternalClient>" << local_ip << "</NewInternalClient>"
         << "<NewEnabled>1</NewEnabled>"
         << "<NewPortMappingDescription>p2p-derp</NewPortMappingDescription>"
         << "<NewLeaseDuration>" << lifetime_sec << "</NewLeaseDuration>"
         << "</u:AddPortMapping></s:Body></s:Envelope>";
    std::string rsp;
    if (!http_post(ctrl, "urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping",
                   soap.str(), rsp, timeout_ms))
        return false;
    out.ok = true;
    out.backend = "upnp";
    out.internal_port = int_port;
    out.external_port = int_port;
    out.lifetime_sec = lifetime_sec;
    snprintf(out.external_ip, sizeof(out.external_ip), "%s", local_ip);
    return true;
}

bool portmap_any(uint16_t int_port, uint32_t lifetime_sec, PortMapResult& out) {
    if (portmap_upnp(int_port, lifetime_sec, 500, out)) return true;
    uint32_t gw = 0;
    if (portmap_default_gateway(&gw)) {
        if (portmap_natpmp_ex(gw, 5351, int_port, lifetime_sec, 300, out)) return true;
        if (portmap_pcp_ex(gw, 5351, int_port, lifetime_sec, 300, out)) return true;
    }
    return false;
}

void portmap_host_ports_from_sdp(const char* sdp, std::vector<uint16_t>& ports) {
    if (!sdp) return;
    const char* p = sdp;
    while (p && *p) {
        const char* line = p;
        const char* nl = strchr(p, '\n');
        std::string s = nl ? std::string(line, nl - line) : std::string(line);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        p = nl ? nl + 1 : nullptr;
        if (s.find("typ host") == std::string::npos &&
            s.find(" typ host") == std::string::npos)
            continue;
        if (s.find("127.0.0.1") != std::string::npos) continue;
        // candidate:... <ip> <port> typ host
        auto typ = s.find("typ host");
        if (typ == std::string::npos) continue;
        // 往前找最后一个数字端口
        size_t i = typ;
        while (i > 0 && s[i - 1] == ' ') i--;
        size_t end = i;
        while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') i--;
        if (i >= end) continue;
        int port = atoi(s.c_str() + i);
        if (port <= 0 || port > 65535) continue;
        uint16_t up = (uint16_t)port;
        bool dup = false;
        for (auto x : ports) if (x == up) { dup = true; break; }
        if (!dup) ports.push_back(up);
    }
}

} // namespace p2p
