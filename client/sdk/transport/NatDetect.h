#ifndef P2P_SDK_TRANSPORT_NAT_DETECT_H
#define P2P_SDK_TRANSPORT_NAT_DETECT_H

// 客户端 NAT 类型自检（状态机，由客户端事件循环驱动）：
//   相位 MAIN：向主 socket 发探测，收集主/备两 socket 的应答（server_index=0/1）
//      均收到         → 全锥型（NAT 放行另一端口回包）
//      仅收到 index=0 → 端口受限或对称 → 进入相位 ALT
//   相位 ALT：向备用 socket 发探测，比较两次请求观察到的公网映射端口：
//      相同 → 端口受限锥型；不同 → 对称型
//   注意：必须用业务同一本地 socket 发送，NAT 映射才一致，故收包在事件循环内完成。

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <functional>

#include "common/ProtoDef.h"
#include "client/sdk/proto/Codec.h"

namespace p2p {

class NatDetect {
public:
    enum class Phase { Idle, Main, Alt, Done };

    using SendFn = std::function<int(const sockaddr_in&, const uint8_t*, size_t)>;

    // 启动：向主 socket 发送探测（server_ip 主服务器 IP）
    void start(const char* server_ip, uint16_t main_port,
               const SendFn& send, uint64_t now_ms) {
        phase_ = Phase::Main;
        mapped_port_main_ = 0;
        got_main_idx0_ = got_main_idx1_ = false;
        got_alt_ = false;
        deadline_ = now_ms + main_timeout_ms_;
        memset(&mapped_ip_main_, 0, sizeof(mapped_ip_main_));
        send_req(send, server_ip, main_port);
    }

    // 事件循环收到 NAT_DETECT_RSP 时喂入（raw 字节，按相位判定归属）：
    //   Main 相位收主/备两 socket 应答（server_index=0/1）；
    //   Alt  相位收备用 socket 应答（server_index=0）
    void feed(const uint8_t* rsp, size_t len) {
        if (len < 3) return;
        uint8_t idx = rsp[0];
        uint16_t mapped_port = (uint16_t)((rsp[35] << 8) | rsp[36]);

        if (phase_ == Phase::Main) {
            if (idx == 0) {
                got_main_idx0_ = true;
                memcpy(mapped_ip_main_, rsp + 19, 16);
                mapped_port_main_ = mapped_port;
            } else if (idx == 1) {
                got_main_idx1_ = true;
            }
            if (got_main_idx0_ && got_main_idx1_) {
                nattype_ = NAT_FULL_CONE;     // 另一 socket 的回包可到达
                phase_ = Phase::Done;
            }
        } else if (phase_ == Phase::Alt) {
            if (idx == 0) {
                got_alt_ = true;
                mapped_port_alt_ = mapped_port;
                classify();
            }
        }
    }

    // 周期性驱动（事件循环每 tick 调用）：超时推进相位
    void tick(const SendFn& send, const char* server_ip,
              uint16_t main_port, uint16_t alt_port, uint64_t now_ms) {
        if (phase_ == Phase::Main && now_ms >= deadline_ + retry_ms_) {
            if (!got_main_idx0_) {           // 主 socket 探测都未回 → 重试
                send_req(send, server_ip, main_port);
                deadline_ = now_ms + main_timeout_ms_;
            } else {                         // 仅 index=0 → 进入 ALT 相位
                phase_ = Phase::Alt;
                send_req(send, server_ip, alt_port);
                deadline_ = now_ms + main_timeout_ms_;
            }
        } else if (phase_ == Phase::Alt && now_ms >= deadline_ + retry_ms_) {
            if (got_alt_) { classify(); }
            else {
                send_req(send, server_ip, alt_port);
                deadline_ = now_ms + main_timeout_ms_;
            }
        }
    }

    bool done() const { return phase_ == Phase::Done; }
    uint8_t nattype() const { return nattype_; }
    const char* mapped_ip() const { return mapped_ip_main_; }
    uint16_t mapped_port() const { return mapped_port_main_; }

    void set_timeouts(uint32_t main_timeout_ms, uint32_t retry_ms) {
        main_timeout_ms_ = main_timeout_ms;
        retry_ms_ = retry_ms;
    }

private:
    void send_req(const SendFn& send, const char* ip, uint16_t port) {
        sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) return;
        to.sin_port = htons(port);
        uint8_t buf[8];
        int n = codec_write_head(buf, MSG_NAT_DETECT_REQ, 0);
        send(to, buf, (size_t)n);
    }

    void classify() {
        if (got_alt_ && mapped_port_alt_ == mapped_port_main_)
            nattype_ = NAT_PORT_RESTRICTED;
        else
            nattype_ = NAT_SYMMETRIC;
        phase_ = Phase::Done;
    }

    Phase   phase_ = Phase::Idle;
    uint8_t nattype_ = NAT_UNKNOWN;
    bool    got_main_idx0_ = false;
    bool    got_main_idx1_ = false;
    bool    got_alt_ = false;
    char    mapped_ip_main_[16] = {0};
    uint16_t mapped_port_main_ = 0;
    uint16_t mapped_port_alt_ = 0;
    uint64_t deadline_ = 0;
    uint32_t main_timeout_ms_ = 2500;
    uint32_t retry_ms_ = 500;
};

} // namespace p2p

#endif // P2P_SDK_TRANSPORT_NAT_DETECT_H
