#ifndef P2P_STATUS_SERVER_H
#define P2P_STATUS_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// 轻量状态服务（对标 n2n 管理端口）：TCP HTTP/1.0
//   GET /         → JSON 快照（任意非 /metrics 路径同样返回 JSON，兼容旧客户端）
//   GET /metrics  → Prometheus 文本（P6）
// ---------------------------------------------------------------------------
class StatusServer {
public:
    StatusServer();
    ~StatusServer();

    // json_provider / metrics_provider 由调用方提供线程安全的状态快照
    // allow 空=不限制（学 EasyTier --rpc-portal-whitelist）
    int  init(uint16_t port,
              std::function<std::string()> json_provider,
              std::function<std::string()> metrics_provider = {},
              std::vector<std::string> allow = {});
    void run();
    void request_stop() { running_ = false; }
    bool running() const { return running_; }

private:
    int      sock_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::function<std::string()> json_provider_;
    std::function<std::string()> metrics_provider_;
    std::vector<std::string> allow_;
};

} // namespace p2p

#endif // P2P_STATUS_SERVER_H
