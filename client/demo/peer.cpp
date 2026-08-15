// peer.cpp：客户端 SDK 演示（P2PClient 门面）
// 用法：
//   发起方： peer <NatServerIP> <NatServerPort> <UUID> <对端UUID> [ProxyIP] [ProxyPort]
//   等待方： peer <NatServerIP> <NatServerPort> <UUID>            [ProxyIP] [ProxyPort]
//   -s <secret>  鉴权密钥（可选）
#include "client/sdk/api/P2PClient.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

using namespace p2p;

static std::atomic<bool> g_quit{false};

static const char* nattype_str(uint8_t t) {
    switch (t) {
    case NAT_FULL_CONE:       return "full-cone";
    case NAT_PORT_RESTRICTED: return "port-restricted";
    case NAT_SYMMETRIC:       return "symmetric";
    default:                  return "unknown";
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <NatServerIP> <NatServerPort> <UUID> [peerUUID] "
                        "[ProxyIP] [ProxyPort] [-s secret] [-k authkey_hex] [-relay]\n",
                argv[0]);
        return 1;
    }
    std::vector<std::string> pos;      // 位置参数
    std::string secret;
    std::string auth_key_hex;          // 每 UID AuthKey（uidgen 签发，设备侧凭据）
    bool force_relay = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) secret = argv[++i];
        else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) auth_key_hex = argv[++i];
        else if (strcmp(argv[i], "-relay") == 0) force_relay = true;
        else pos.push_back(argv[i]);
    }
    if (pos.size() < 3) {
        fprintf(stderr, "missing positional args (ip port uuid [peer] [proxyip proxyport])\n");
        return 1;
    }
    std::string nat_ip = pos[0];
    uint16_t    nat_port = (uint16_t)atoi(pos[1].c_str());
    std::string uuid = pos[2];

    // 等待方用法允许省略 peerUUID，直接跟 ProxyIP ProxyPort：
    //   peer <nat> <port> <uuid> [peerUUID] [ProxyIP ProxyPort]
    auto looks_like_ipv4 = [](const std::string& s) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        char extra = 0;
        return sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 &&
               a <= 255 && b <= 255 && c <= 255 && d <= 255;
    };
    std::string peer_uuid;
    size_t proxy_idx = 3;
    if (pos.size() > 3) {
        if (looks_like_ipv4(pos[3])) {
            proxy_idx = 3;              // 无对端 UUID，其后即 proxy
        } else {
            peer_uuid = pos[3];
            proxy_idx = 4;
        }
    }

    std::vector<P2PClient::ServerAddr> proxies;
    for (size_t i = proxy_idx; i + 1 < pos.size(); i += 2) {
        P2PClient::ServerAddr a;
        a.ip = pos[i];
        a.port = (uint16_t)atoi(pos[i + 1].c_str());
        if (!a.ip.empty() && a.port != 0) proxies.push_back(a);
    }

    P2PClient::Config cfg;
    cfg.uuid = uuid;
    cfg.secret = secret;
    cfg.auth_key_hex = auth_key_hex;
    cfg.nat_servers.push_back({nat_ip, nat_port});
    cfg.proxy_servers = proxies;
    cfg.force_relay = force_relay;

    P2PClient client;
    std::atomic<int> connected{0};
    std::atomic<bool> replied{false};

    client.on_ready = [&](const char* pub_ip, uint16_t pub_port, uint8_t nt) {
        printf("[peer] ready pub=%s:%d nattype=%s\n",
               pub_ip, pub_port, nattype_str(nt));
        fflush(stdout);
        // 注册完成后再发起连接，避免 CONNECT 时本端尚未上线
        if (!peer_uuid.empty()) {
            printf("[peer] connecting to %s...\n", peer_uuid.c_str());
            fflush(stdout);
            client.connect(peer_uuid);
        }
    };
    client.on_connected = [&](const std::string& peer, bool relay) {
        connected = 1;
        printf("[peer] CONNECTED to %s via %s\n", peer.c_str(),
               relay ? "relay" : "direct");
        fflush(stdout);
        char msg[64];
        snprintf(msg, sizeof(msg), "hello from %s", uuid.c_str());
        client.send(peer, 1, msg, strlen(msg), true);
    };
    client.on_message = [&](const std::string& peer, uint8_t ch,
                            const uint8_t* data, size_t len) {
        printf("[peer] recv from %s ch=%d [%zuB]: %.*s\n", peer.c_str(), ch,
               len, (int)len, (const char*)data);
        fflush(stdout);
        if (!replied.exchange(true)) {       // 仅回复一次，避免回声乒乓
            std::string reply = "reply from " + uuid;
            client.send(peer, 1, reply.data(), reply.size(), true);
        }
    };
    client.on_disconnected = [&](const std::string& peer) {
        printf("[peer] DISCONNECTED from %s\n", peer.c_str());
        fflush(stdout);
    };
    client.on_error = [&](const std::string& err) {
        fprintf(stderr, "[peer] error: %s\n", err.c_str());
        fflush(stderr);
    };

    if (!client.start(cfg)) {
        fprintf(stderr, "failed to start client\n");
        return 1;
    }
    printf("[peer] started uuid=%s local_port=%d\n", uuid.c_str(),
           client.local_port());
    fflush(stdout);

    uint8_t last_nat = NAT_UNKNOWN;
    uint64_t last_etx = 0, last_erx = 0;
    while (!g_quit.load()) {
        uint8_t nt = client.nat_type();
        if (nt != last_nat) {
            last_nat = nt;
            printf("[peer] nattype=%s\n", nattype_str(nt));
            fflush(stdout);
        }
        uint64_t etx = client.tunnel_enc_tx(), erx = client.tunnel_enc_rx();
        if (etx != last_etx || erx != last_erx) {
            last_etx = etx;
            last_erx = erx;
            printf("[peer] tunnel-enc tx=%llu rx=%llu\n",
                   (unsigned long long)etx, (unsigned long long)erx);
            fflush(stdout);
        }
        p2p::plat_sleep_ms(200);
    }
    client.stop();
    return 0;
}
