// peer.cpp：测试对端客户端
// 演示完整流程：注册/心跳 -> CONNECT 请求 -> UDP 打洞直连 -> 失败走中继
//
// 用法：
//   发起方： peer <NatServerIP> <NatServerPort> <UUID> <对端UUID> [ProxyIP] [ProxyPort]
//   等待方： peer <NatServerIP> <NatServerPort> <UUID>            [ProxyIP] [ProxyPort]
#include "ProtoDef.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace p2p;

struct PeerClient {
    int fd = -1;
    std::string my_uuid;
    std::string peer_uuid;
    sockaddr_in nat{};
    sockaddr_in proxy{};
    bool have_proxy = false;

    sockaddr_in my_pub{};
    bool have_pub = false;

    sockaddr_in remote{};      // 直连目标地址
    bool have_remote = false;

    bool direct_ok = false;
    bool relay_ok = false;
    bool relay_registered = false;
    bool register_sent = false;

    bool sent_connect = false;
    int  ping_seq = 0;
    time_t next_hb = 0;
    time_t next_punch = 0;
    time_t direct_deadline = 0;
    time_t next_relay_ping = 0;
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static std::string addr_str(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(a.sin_port));
    return buf;
}

static int sendto_raw(PeerClient& c, const sockaddr_in& to, const uint8_t* buf, size_t len) {
    return (int)sendto(c.fd, buf, len, 0, (const sockaddr*)&to, sizeof(to));
}

static int send_msg(PeerClient& c, const sockaddr_in& to, uint8_t msg_id,
                    const void* payload, size_t plen) {
    uint8_t buf[2048];
    if (plen > sizeof(buf) - sizeof(MsgHead)) return -1;
    MsgHead h;
    h.magic = htons(NAT_MAGIC);
    h.version = PROTO_VER;
    h.msg_id = msg_id;
    h.length = htonl((uint32_t)plen);
    memcpy(buf, &h, sizeof(h));
    if (plen) memcpy(buf + sizeof(h), payload, plen);
    return sendto_raw(c, to, buf, sizeof(h) + plen);
}

static void send_heartbeat(PeerClient& c) {
    UuidReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    req.dev_type = 2;
    sockaddr_in self;
    socklen_t sl = sizeof(self);
    if (getsockname(c.fd, (sockaddr*)&self, &sl) == 0) req.lan_port = self.sin_port;
    send_msg(c, c.nat, MSG_HEARTBEAT_REQ, &req, sizeof(req));
    printf("[peer %s] send HEARTBEAT_REQ\n", c.my_uuid.c_str());
    c.next_hb = time(nullptr) + 5;
}

static void send_connect(PeerClient& c) {
    ConnectReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    strncpy(req.dst_uuid, c.peer_uuid.c_str(), MAX_UUID_LEN);
    send_msg(c, c.nat, MSG_CONNECT_REQ, &req, sizeof(req));
    c.sent_connect = true;
    printf("[peer %s] send CONNECT_REQ -> dst[%s]\n", c.my_uuid.c_str(), c.peer_uuid.c_str());
}

static void send_direct(PeerClient& c, uint8_t type, const char* msg) {
    if (!c.have_remote) return;
    PeerData d;
    memset(&d, 0, sizeof(d));
    memcpy(d.magic, "PDAT", 4);
    d.type = type;
    d.seq = (uint8_t)(c.ping_seq++);
    strncpy(d.src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    strncpy(d.msg, msg, sizeof(d.msg) - 1);
    sendto_raw(c, c.remote, reinterpret_cast<const uint8_t*>(&d), sizeof(d));
}

static void register_proxy(PeerClient& c) {
    ProxyRegReq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    send_msg(c, c.proxy, MSG_PROXY_REGISTER_REQ, &req, sizeof(req));
    printf("[peer %s] send PROXY_REGISTER_REQ to [%s]\n", c.my_uuid.c_str(), addr_str(c.proxy).c_str());
}

static void send_relay(PeerClient& c, uint8_t type, const char* msg) {
    if (!c.have_proxy || !c.relay_registered) return;
    uint8_t buf[sizeof(MsgHead) + sizeof(RelayFrame) + sizeof(PeerData)];
    MsgHead h;
    h.magic = htons(NAT_MAGIC);
    h.version = PROTO_VER;
    h.msg_id = MSG_PROXY_RELAY_DATA;
    h.length = htonl(sizeof(RelayFrame) + sizeof(PeerData));
    memcpy(buf, &h, sizeof(h));

    RelayFrame* f = reinterpret_cast<RelayFrame*>(buf + sizeof(h));
    memset(f, 0, sizeof(RelayFrame));
    strncpy(f->src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    strncpy(f->dst_uuid, c.peer_uuid.c_str(), MAX_UUID_LEN);

    PeerData* d = reinterpret_cast<PeerData*>(buf + sizeof(h) + sizeof(RelayFrame));
    memset(d, 0, sizeof(PeerData));
    memcpy(d->magic, "PDAT", 4);
    d->type = type;
    d->seq = (uint8_t)(c.ping_seq++);
    strncpy(d->src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
    strncpy(d->msg, msg, sizeof(d->msg) - 1);

    sendto_raw(c, c.proxy, buf, sizeof(buf));
}

static void handle_recv(PeerClient& c, const uint8_t* buf, size_t len, const sockaddr_in& from) {
    if (len < 2) return;

    // 直连数据：PeerData（无报文头）
    if (buf[0] == 'P' && buf[1] == 'D') {
        PeerData d;
        memset(&d, 0, sizeof(d));
        memcpy(&d, buf, len < sizeof(d) ? len : sizeof(d));

        if (!c.have_remote) { c.remote = from; c.have_remote = true; }

        if (d.type == 2) {          // PUNCH
            printf("[peer %s] recv PUNCH from [%s]\n", c.my_uuid.c_str(), addr_str(from).c_str());
            return;
        }
        if (d.type == 0) {          // PING -> 回 PONG
            printf("[peer %s] recv PING, reply PONG direct\n", c.my_uuid.c_str());
            c.direct_ok = true;
            send_direct(c, 1, "pong(direct)");
        } else if (d.type == 1) {   // PONG
            c.direct_ok = true;
            printf("[peer %s] recv PONG direct, DIRECT P2P OK!\n", c.my_uuid.c_str());
        }
        return;
    }

    if (len < sizeof(MsgHead)) return;
    MsgHead h;
    memcpy(&h, buf, sizeof(h));
    if (ntohs(h.magic) != NAT_MAGIC) return;
    uint32_t plen = ntohl(h.length);
    if (plen > len - sizeof(MsgHead)) return;
    const uint8_t* p = buf + sizeof(MsgHead);

    switch (h.msg_id) {
    case MSG_HEARTBEAT_RSP: {
        if (plen < sizeof(ExtInfoRsp)) break;
        ExtInfoRsp rsp;
        memcpy(&rsp, p, sizeof(rsp));
        printf("[peer %s] HEARTBEAT_RSP pub[%s:%d]\n", c.my_uuid.c_str(), rsp.pub_ip, ntohs(rsp.pub_port));
        c.have_pub = true;
        break;
    }
    case MSG_CONNECT_ACK: {
        if (plen < sizeof(ConnectAck)) break;
        ConnectAck ack;
        memcpy(&ack, p, sizeof(ack));
        printf("[peer %s] CONNECT_ACK result[%d]\n", c.my_uuid.c_str(), ack.result);
        if (ack.result != 0) break;
        inet_pton(AF_INET, ack.dst_pub_ip, &c.remote.sin_addr);
        c.remote.sin_port = ack.dst_pub_port;
        c.remote.sin_family = AF_INET;
        c.have_remote = true;
        c.direct_deadline = time(nullptr) + 6;
        if (ack.proxy_ip[0] && !c.have_proxy) {
            inet_pton(AF_INET, ack.proxy_ip, &c.proxy.sin_addr);
            c.proxy.sin_port = ack.proxy_port;
            c.proxy.sin_family = AF_INET;
            c.have_proxy = true;
        }
        printf("[peer %s] CONNECT_ACK dst pub[%s] proxy[%s:%d]\n", c.my_uuid.c_str(),
               addr_str(c.remote).c_str(), ack.proxy_ip, ntohs(ack.proxy_port));
        break;
    }
    case MSG_CONNECT_INVITE: {
        if (plen < sizeof(ConnectInvite)) break;
        ConnectInvite inv;
        memcpy(&inv, p, sizeof(inv));
        c.peer_uuid = inv.src_uuid;
        inet_pton(AF_INET, inv.src_pub_ip, &c.remote.sin_addr);
        c.remote.sin_port = inv.src_pub_port;
        c.remote.sin_family = AF_INET;
        c.have_remote = true;
        if (inv.proxy_ip[0] && !c.have_proxy) {
            inet_pton(AF_INET, inv.proxy_ip, &c.proxy.sin_addr);
            c.proxy.sin_port = inv.proxy_port;
            c.proxy.sin_family = AF_INET;
            c.have_proxy = true;
        }
        printf("[peer %s] CONNECT_INVITE from[%s] pub[%s] proxy[%s:%d]\n", c.my_uuid.c_str(),
               inv.src_uuid, addr_str(c.remote).c_str(), inv.proxy_ip, ntohs(inv.proxy_port));
        c.next_punch = time(nullptr);   // 立即反向打洞
        break;
    }
    case MSG_PROXY_REGISTER_RSP: {
        if (plen < sizeof(ProxyRegRsp)) break;
        ProxyRegRsp rsp;
        memcpy(&rsp, p, sizeof(rsp));
        if (rsp.result == 0) {
            c.relay_registered = true;
            printf("[peer %s] PROXY_REGISTER_RSP ok, pub[%s:%d]\n", c.my_uuid.c_str(),
                   rsp.pub_ip, ntohs(rsp.pub_port));
        } else {
            printf("[peer %s] PROXY_REGISTER_RSP failed result[%d]\n", c.my_uuid.c_str(), rsp.result);
        }
        break;
    }
    case MSG_PROXY_RELAY_DATA: {
        if (plen < sizeof(RelayFrame)) break;
        RelayFrame* f = reinterpret_cast<RelayFrame*>(const_cast<uint8_t*>(p));
        PeerData* d = reinterpret_cast<PeerData*>(const_cast<uint8_t*>(p + sizeof(RelayFrame)));
        if (strncmp(f->dst_uuid, c.my_uuid.c_str(), MAX_UUID_LEN) != 0) break;

        c.relay_ok = true;
        printf("[peer %s] recv RELAY frame type[%d]\n", c.my_uuid.c_str(), d->type);
        if (d->type == 0) {          // 中继 PING -> 回中继 PONG
            uint8_t rbuf[sizeof(MsgHead) + sizeof(RelayFrame) + sizeof(PeerData)];
            MsgHead hh;
            hh.magic = htons(NAT_MAGIC);
            hh.version = PROTO_VER;
            hh.msg_id = MSG_PROXY_RELAY_DATA;
            hh.length = htonl(sizeof(RelayFrame) + sizeof(PeerData));
            memcpy(rbuf, &hh, sizeof(hh));

            RelayFrame* rf = reinterpret_cast<RelayFrame*>(rbuf + sizeof(hh));
            memset(rf, 0, sizeof(RelayFrame));
            strncpy(rf->src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
            strncpy(rf->dst_uuid, c.peer_uuid.c_str(), MAX_UUID_LEN);

            PeerData* pd = reinterpret_cast<PeerData*>(rbuf + sizeof(hh) + sizeof(RelayFrame));
            memset(pd, 0, sizeof(PeerData));
            memcpy(pd->magic, "PDAT", 4);
            pd->type = 1;
            strncpy(pd->src_uuid, c.my_uuid.c_str(), MAX_UUID_LEN);
            strncpy(pd->msg, "pong(relay)", sizeof(pd->msg) - 1);

            sendto_raw(c, c.proxy, rbuf, sizeof(rbuf));
            printf("[peer %s] reply RELAY PONG\n", c.my_uuid.c_str());
        }
        break;
    }
    case MSG_GET_DEV_LIST_RSP: {
        if (plen < sizeof(DevListRsp)) break;
        DevListRsp* rsp = reinterpret_cast<DevListRsp*>(const_cast<uint8_t*>(p));
        printf("[peer %s] DEV_LIST total[%d] count[%d]\n", c.my_uuid.c_str(),
               ntohs(rsp->total), ntohs(rsp->count));
        break;
    }
    case MSG_GET_SERVER_LIST_RSP: {
        if (plen < sizeof(ServerListRsp)) break;
        ServerListRsp* rsp = reinterpret_cast<ServerListRsp*>(const_cast<uint8_t*>(p));
        printf("[peer %s] SERVER_LIST nat[%d] proxy[%d]\n", c.my_uuid.c_str(),
               ntohs(rsp->nat_count), ntohs(rsp->proxy_count));
        break;
    }
    default:
        break;
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage:\n"
               "  initiator: %s <NatServerIP> <NatServerPort> <UUID> <DstUUID> [ProxyIP] [ProxyPort]\n"
               "  waiter   : %s <NatServerIP> <NatServerPort> <UUID>            [ProxyIP] [ProxyPort]\n",
               argv[0], argv[0]);
        return 1;
    }

    PeerClient c;
    c.my_uuid = argv[3];
    c.peer_uuid = (argc >= 5) ? argv[4] : "";
    bool is_initiator = !c.peer_uuid.empty();

    inet_pton(AF_INET, argv[1], &c.nat.sin_addr);
    c.nat.sin_family = AF_INET;
    c.nat.sin_port = htons((uint16_t)atoi(argv[2]));

    if (argc >= 7) {
        inet_pton(AF_INET, argv[5], &c.proxy.sin_addr);
        c.proxy.sin_family = AF_INET;
        c.proxy.sin_port = htons((uint16_t)atoi(argv[6]));
        c.have_proxy = true;
    }

    c.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c.fd < 0) { perror("socket"); return 1; }
    sockaddr_in self;
    memset(&self, 0, sizeof(self));
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_ANY);
    self.sin_port = htons(0);
    if (bind(c.fd, (const sockaddr*)&self, sizeof(self)) != 0) { perror("bind"); return 1; }
    set_nonblock(c.fd);

    printf("[peer %s] start, nat[%s] %s\n", c.my_uuid.c_str(),
           addr_str(c.nat).c_str(), is_initiator ? ("-> connect " + c.peer_uuid).c_str() : "(waiter)");
    send_heartbeat(c);

    time_t deadline = time(nullptr) + 40;
    while (time(nullptr) < deadline && !(c.direct_ok || c.relay_ok)) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(c.fd, &rf);
        timeval tv{0, 300000};
        int r = select(c.fd + 1, &rf, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(c.fd, &rf)) {
            uint8_t buf[2048];
            sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(c.fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n > 0) handle_recv(c, buf, (size_t)n, from);
        }

        time_t now = time(nullptr);
        if (now >= c.next_hb) send_heartbeat(c);

        if (is_initiator) {
            if (c.have_pub && !c.sent_connect) send_connect(c);

            if (c.have_remote && now < c.direct_deadline) {
                if (now >= c.next_punch) {
                    c.next_punch = now + 1;
                    send_direct(c, 2, "punch");
                    send_direct(c, 0, "hello p2p");
                    printf("[peer %s] punching -> [%s]\n", c.my_uuid.c_str(), addr_str(c.remote).c_str());
                }
            } else if (c.have_remote && now >= c.direct_deadline) {
                if (!c.relay_registered && !c.register_sent) {
                    c.register_sent = true;
                    if (c.have_proxy) register_proxy(c);
                    else printf("[peer %s] no proxy available, relay skip\n", c.my_uuid.c_str());
                }
                if (c.relay_registered && now >= c.next_relay_ping) {
                    c.next_relay_ping = now + 1;
                    send_relay(c, 0, "hello via proxy");
                    printf("[peer %s] try RELAY ping\n", c.my_uuid.c_str());
                }
            }
        } else {
            if (c.have_proxy && !c.relay_registered && !c.register_sent && c.have_remote) {
                c.register_sent = true;
                register_proxy(c);
            }
            if (c.have_remote && now >= c.next_punch) {
                c.next_punch = now + 1;
                send_direct(c, 2, "punch-back");
            }
        }
    }

    printf("[peer %s] RESULT: %s\n", c.my_uuid.c_str(),
           c.direct_ok ? "DIRECT P2P OK" : (c.relay_ok ? "RELAY P2P OK" : "FAILED"));
    close(c.fd);
    return 0;
}
