#include "StatusServer.h"
#include "Log.h"

#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace p2p {

StatusServer::StatusServer() {}
StatusServer::~StatusServer() {
    if (sock_ >= 0) close(sock_);
}

int StatusServer::init(uint16_t port, std::function<std::string()> json_provider) {
    port_ = port;
    provider_ = std::move(json_provider);

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) { perror("[StatusServer] socket"); return -1; }
    int on = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (bind(sock_, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("[StatusServer] bind");
        close(sock_);
        sock_ = -1;
        return -1;
    }
    if (listen(sock_, 8) != 0) {
        perror("[StatusServer] listen");
        close(sock_);
        sock_ = -1;
        return -1;
    }
    running_ = true;
    LOGI("StatusServer", "listening on port %u", (unsigned)port_);
    return 0;
}

void StatusServer::run() {
    while (running_) {
        sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int fd = accept(sock_, (sockaddr*)&cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!running_) break;
            continue;
        }

        // 简化：读掉请求头（带超时），回 HTTP/1.0 JSON，关闭
        struct timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[1024];
        recv(fd, buf, sizeof(buf), 0);  // 忽略请求体

        std::string body = provider_ ? provider_() : "{}";
        std::string resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "\r\n" + body;
        send(fd, resp.data(), resp.size(), 0);
        close(fd);
    }
}

} // namespace p2p
