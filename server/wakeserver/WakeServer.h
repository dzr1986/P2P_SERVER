#ifndef P2P_WAKE_SERVER_H
#define P2P_WAKE_SERVER_H

#include "Net.h"
#include "ProtoDef.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

// 低功耗唤醒保活服务：设备以极低频 UDP 报到，客户端/NatServer 触发时
// 向设备上次公网地址发送 POKE，设备收到后全速上线走正常 CONNECT。
class WakeServer {
public:
    int  init(uint16_t port, const std::string& secret, uint32_t ttl_sec = 180);
    int  run();
    void request_stop() { running_ = false; }

private:
    void handle(const uint8_t* data, size_t len, const sockaddr_in& from);
    void on_keepalive(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void on_trigger(const uint8_t* p, size_t plen, const sockaddr_in& from);
    void send_result(const sockaddr_in& to, uint8_t result, const char* uuid);
    void send_poke(const sockaddr_in& to, const char* uuid);
    void sweep_expired();
    bool verify_hmac(const char* uuid, const uint8_t mac[32]) const;

    UdpFd sock_;
    uint16_t port_ = 0;
    std::string secret_;
    uint32_t ttl_sec_ = 180;
    std::atomic<bool> running_{false};

    struct Entry {
        sockaddr_in addr{};
        time_t last_seen = 0;
    };
    std::mutex mu_;
    std::unordered_map<std::string, Entry> table_;
};

} // namespace p2p

#endif // P2P_WAKE_SERVER_H
