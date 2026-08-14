#ifndef P2P_STATUS_SERVER_H
#define P2P_STATUS_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace p2p {

// ---------------------------------------------------------------------------
// 轻量状态服务（对标 n2n 管理端口）：TCP HTTP/1.0，返回 JSON
// 用法：/ 返回在线数/表项数/pps/计数等；任意请求均返回同一快照
// ---------------------------------------------------------------------------
class StatusServer {
public:
    StatusServer();
    ~StatusServer();

    // json_provider 由调用方提供线程安全的状态快照
    int  init(uint16_t port, std::function<std::string()> json_provider);
    void run();
    void request_stop() { running_ = false; }
    bool running() const { return running_; }

private:
    int      sock_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::function<std::string()> provider_;
};

} // namespace p2p

#endif // P2P_STATUS_SERVER_H
