#ifndef P2P_SDK_TRANSPORT_NAT_DETECT_H
#define P2P_SDK_TRANSPORT_NAT_DETECT_H

// 客户端 NAT 自检（状态机，由事件循环驱动）：
//   相位 MAIN：向主 socket 发探测，收集主/备两 socket 的应答（server_index=0/1）
//      均收到         → filter=none（全锥）；映射按 EIM 记（无第二公网 IP 时无法分 ADM）
//      仅收到 index=0 → 过滤为 addr/port → 进入相位 ALT
//   相位 ALT：向备用 socket 发探测，比较两次请求观察到的公网映射端口：
//      相同 → mapping=EIM + filter=port（端口受限锥型）
//      不同 → mapping=EDM（对称）；若应答带 probe_port 则进入 PREDICT
//   相位 PREDICT：向第三探测口发一次，端口差相等且非 0 → NAT4E step
//   必须用业务同一本地 socket 发送，NAT 映射才一致。

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
    enum class Phase { Idle, Main, Alt, Predict, Done };

    using SendFn = std::function<int(const sockaddr_in&, const uint8_t*, size_t)>;

    void start(const char* server_ip, uint16_t main_port,
               const SendFn& send, uint64_t now_ms) {
        phase_ = Phase::Main;
        mapped_port_main_ = mapped_port_alt_ = mapped_port_probe_ = 0;
        probe_port_ = 0;
        port_step_ = 0;
        mapping_ = NAT_MAP_UNKNOWN;
        filter_ = NAT_FLT_UNKNOWN;
        nattype_ = NAT_UNKNOWN;
        got_main_idx0_ = got_main_idx1_ = false;
        got_alt_ = got_probe_ = false;
        deadline_ = now_ms + main_timeout_ms_;
        memset(&mapped_ip_main_, 0, sizeof(mapped_ip_main_));
        send_req(send, server_ip, main_port);
    }

    void feed(const uint8_t* rsp, size_t len) {
        NatDetectRsp m{};
        if (!parse_rsp(rsp, len, m)) return;
        const uint8_t idx = m.server_index;
        const uint16_t mapped_port = ntohs(m.mapped_port);
        const uint16_t probe = ntohs(m.probe_port);
        if (probe) probe_port_ = probe;

        if (phase_ == Phase::Main) {
            if (idx == 0) {
                got_main_idx0_ = true;
                memcpy(mapped_ip_main_, m.mapped_ip, sizeof(mapped_ip_main_));
                mapped_port_main_ = mapped_port;
            } else if (idx == 1) {
                got_main_idx1_ = true;
            }
            if (got_main_idx0_ && got_main_idx1_) {
                filter_ = NAT_FLT_NONE;
                mapping_ = NAT_MAP_EIM;
                nattype_ = NAT_FULL_CONE;
                phase_ = Phase::Done;
            }
        } else if (phase_ == Phase::Alt) {
            if (idx == 0 || idx == 1) {
                got_alt_ = true;
                mapped_port_alt_ = mapped_port;
                classify_alt();
            }
        } else if (phase_ == Phase::Predict) {
            if (idx == 2) {
                got_probe_ = true;
                mapped_port_probe_ = mapped_port;
                finish_edm();
            }
        }
    }

    void tick(const SendFn& send, const char* server_ip,
              uint16_t main_port, uint16_t alt_port, uint64_t now_ms) {
        if (phase_ == Phase::Main && now_ms >= deadline_ + retry_ms_) {
            if (!got_main_idx0_) {
                send_req(send, server_ip, main_port);
                deadline_ = now_ms + main_timeout_ms_;
            } else {
                phase_ = Phase::Alt;
                send_req(send, server_ip, alt_port);
                deadline_ = now_ms + main_timeout_ms_;
            }
        } else if (phase_ == Phase::Alt && now_ms >= deadline_ + retry_ms_) {
            if (got_alt_) {
                classify_alt();
            } else {
                send_req(send, server_ip, alt_port);
                deadline_ = now_ms + main_timeout_ms_;
            }
        } else if (phase_ == Phase::Predict && now_ms >= deadline_ + retry_ms_) {
            if (got_probe_) {
                finish_edm();
            } else if (probe_port_) {
                send_req(send, server_ip, probe_port_);
                deadline_ = now_ms + main_timeout_ms_;
            } else {
                finish_edm();
            }
        }
    }

    bool done() const { return phase_ == Phase::Done; }
    uint8_t nattype() const { return nattype_; }
    uint8_t mapping() const { return mapping_; }
    uint8_t filter() const { return filter_; }
    int16_t port_step() const { return port_step_; }
    uint16_t probe_port() const { return probe_port_; }
    const char* mapped_ip() const { return mapped_ip_main_; }
    uint16_t mapped_port() const { return mapped_port_main_; }

    void set_timeouts(uint32_t main_timeout_ms, uint32_t retry_ms) {
        main_timeout_ms_ = main_timeout_ms;
        retry_ms_ = retry_ms;
    }

    // 单测：按与服务器相同的结构体布局装填一条应答
    static void pack_rsp(uint8_t* out, size_t* outlen, uint8_t idx,
                         uint16_t mapped_port, uint16_t main_port,
                         uint16_t alt_port, uint16_t probe_port = 0) {
        NatDetectRsp m{};
        m.server_index = idx;
        m.mapped_port = htons(mapped_port);
        m.main_port = htons(main_port);
        m.alt_port = htons(alt_port);
        m.probe_port = htons(probe_port);
        memcpy(out, &m, sizeof(m));
        if (outlen) *outlen = sizeof(m);
    }

private:
    static bool parse_rsp(const uint8_t* rsp, size_t len, NatDetectRsp& m) {
        if (!rsp || len < 3) return false;
        const size_t n = len < sizeof(m) ? len : sizeof(m);
        memcpy(&m, rsp, n);
        return true;
    }

    void send_req(const SendFn& send, const char* ip, uint16_t port) {
        if (!ip || port == 0) return;
        sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) return;
        to.sin_port = htons(port);
        uint8_t buf[8];
        int n = codec_write_head(buf, MSG_NAT_DETECT_REQ, 0);
        send(to, buf, (size_t)n);
    }

    void classify_alt() {
        if (!got_alt_) return;
        filter_ = NAT_FLT_PORT;
        if (mapped_port_alt_ == mapped_port_main_) {
            mapping_ = NAT_MAP_EIM;
            nattype_ = NAT_PORT_RESTRICTED;
            phase_ = Phase::Done;
            return;
        }
        mapping_ = NAT_MAP_EDM;
        nattype_ = NAT_SYMMETRIC;
        if (probe_port_) {
            phase_ = Phase::Predict;
            deadline_ = 0;   // 下一 tick 立即发第三口
            return;
        }
        finish_edm();
    }

    void finish_edm() {
        mapping_ = NAT_MAP_EDM;
        if (filter_ == NAT_FLT_UNKNOWN) filter_ = NAT_FLT_PORT;
        nattype_ = NAT_SYMMETRIC;
        port_step_ = 0;
        if (got_probe_ && mapped_port_main_ && mapped_port_alt_ && mapped_port_probe_) {
            const int s1 = (int)mapped_port_alt_ - (int)mapped_port_main_;
            const int s2 = (int)mapped_port_probe_ - (int)mapped_port_alt_;
            if (s1 != 0 && s1 == s2 && s1 >= -256 && s1 <= 256)
                port_step_ = (int16_t)s1;
        }
        phase_ = Phase::Done;
    }

    Phase    phase_ = Phase::Idle;
    uint8_t  nattype_ = NAT_UNKNOWN;
    uint8_t  mapping_ = NAT_MAP_UNKNOWN;
    uint8_t  filter_ = NAT_FLT_UNKNOWN;
    int16_t  port_step_ = 0;
    bool     got_main_idx0_ = false;
    bool     got_main_idx1_ = false;
    bool     got_alt_ = false;
    bool     got_probe_ = false;
    char     mapped_ip_main_[16] = {0};
    uint16_t mapped_port_main_ = 0;
    uint16_t mapped_port_alt_ = 0;
    uint16_t mapped_port_probe_ = 0;
    uint16_t probe_port_ = 0;
    uint64_t deadline_ = 0;
    uint32_t main_timeout_ms_ = 2500;
    uint32_t retry_ms_ = 500;
};

} // namespace p2p

#endif // P2P_SDK_TRANSPORT_NAT_DETECT_H
