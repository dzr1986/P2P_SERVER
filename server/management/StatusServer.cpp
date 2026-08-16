#include "server/management/StatusServer.h"
#include "server/config/CfgFile.h"
#include "core/foundation/Log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace p2p {

StatusServer::StatusServer() {}
StatusServer::~StatusServer() {
    if (sock_ >= 0) close(sock_);
}

int StatusServer::init(uint16_t port,
                       std::function<std::string()> json_provider,
                       std::function<std::string()> metrics_provider,
                       std::vector<std::string> allow) {
    port_ = port;
    json_provider_ = std::move(json_provider);
    metrics_provider_ = std::move(metrics_provider);
    allow_ = std::move(allow);

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
    LOGI("StatusServer", "listening on port %u (/ and /metrics) allow=%zu",
         (unsigned)port_, allow_.size());
    return 0;
}

static std::string http_path(const char* buf, ssize_t n) {
    if (!buf || n < 4) return "/";
    const char* p = buf;
    const char* end = buf + n;
    while (p < end && *p != ' ') p++;
    if (p >= end) return "/";
    p++;
    const char* start = p;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    if (p == start) return "/";
    return std::string(start, p);
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
        if (!ipv4_in_allow_list(cli.sin_addr.s_addr, allow_)) {
            const char k403[] =
                "HTTP/1.0 403 Forbidden\r\nContent-Length: 9\r\n"
                "Connection: close\r\n\r\nforbidden";
            send(fd, k403, sizeof(k403) - 1, 0);
            close(fd);
            continue;
        }

        struct timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[1024];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) n = 0;
        buf[n] = 0;

        const std::string path = http_path(buf, n);
        const bool metrics = (path == "/metrics" || path.rfind("/metrics?", 0) == 0);
        std::string body;
        const char* ctype = "application/json";
        if (metrics && metrics_provider_) {
            body = metrics_provider_();
            ctype = "text/plain; version=0.0.4; charset=utf-8";
        } else {
            body = json_provider_ ? json_provider_() : "{}";
        }
        std::string resp =
            std::string("HTTP/1.0 200 OK\r\n") +
            "Content-Type: " + ctype + "\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "\r\n" + body;
        send(fd, resp.data(), resp.size(), 0);
        close(fd);
    }
}

} // namespace p2p
