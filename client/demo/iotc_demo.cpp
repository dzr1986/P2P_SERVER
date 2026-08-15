// iotc_demo：IOTC/AV/RDT/Tunnel 通道层演示（计划书 P2/P4 验收）
//   设备端：等待连入，四通道并发回声 + 本地 TCP echo 经 P2PTunnel 映射
//   客户端：连接设备，四通道并发发送并校验回声，再验证 TCP 隧道
// 用法：
//   iotc_demo device <NatIP> <NatPort> <UID>            [-s secret] [-k key]
//   iotc_demo client <NatIP> <NatPort> <UID> <DevUID>   [-s secret] [-k key]
#include "client/sdk/iotc/IOTC.h"
#include "client/sdk/iotc/AVAPIs.h"
#include "client/sdk/iotc/RDTAPIs.h"
#include "client/sdk/iotc/P2PTunnelAPIs.h"
#include "common/Net.h"
#include "common/ProtoDef.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint8_t kVideoCh = 1;
constexpr uint8_t kAudioCh = 2;
constexpr uint8_t kRdtCh = 3;
constexpr uint8_t kTunCh = 4;
constexpr uint16_t kCmdPing = 0x10;
constexpr int kVideoLen = 5000;   // > 4 个切片，验证分片重组
constexpr int kAudioLen = 300;
constexpr int kRdtLen = 2500;     // 跨两个隧道包，验证字节流拼接
constexpr int kVideoFrames = 5;
constexpr int kAudioFrames = 5;

void fill_pat(uint8_t* p, int n, uint8_t seed) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(seed + i);
}

bool same_pat(const uint8_t* p, int n, uint8_t seed) {
    for (int i = 0; i < n; i++) {
        if (p[i] != (uint8_t)(seed + i)) return false;
    }
    return true;
}

uint16_t start_tcp_echo(p2p::TcpFd& listen, std::atomic<bool>& run, std::thread& thr) {
    if (!listen.open() || !listen.set_reuse() || !listen.bind_loopback(0) ||
        !listen.listen()) {
        return 0;
    }
    const uint16_t port = listen.local_port();
    thr = std::thread([&listen, &run] {
        while (run.load()) {
            pollfd pfd{};
            pfd.fd = listen.fd();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;
            p2p::TcpFd cli = listen.accept_one();
            if (!cli.valid()) continue;
            cli.set_recv_timeout_ms(200);
            uint8_t buf[512];
            for (;;) {
                const ssize_t n = cli.recv_some(buf, sizeof(buf));
                if (n < 0) {
                    if (!run.load()) break;
                    continue;
                }
                if (n == 0) break;
                cli.send_all(buf, (size_t)n);
            }
        }
    });
    return port;
}

int run_device(const char* uid, const char* secret, const char* key) {
    int r = IOTC_Login(uid, secret, key, 8000);
    if (r != IOTC_ER_NoERROR) {
        fprintf(stderr, "[iotc] device login err=%d\n", r);
        return 1;
    }
    printf("[iotc] device logged in uid=%s\n", uid);
    fflush(stdout);

    const int sid = IOTC_Listen(20000);
    if (sid < 0) {
        fprintf(stderr, "[iotc] device listen err=%d\n", sid);
        return 1;
    }
    IOTCSessionInfo info{};
    IOTC_Session_Check(sid, &info);
    printf("[iotc] device session sid=%d peer=%s relay=%d\n",
           sid, info.peer_uid, info.via_relay);
    fflush(stdout);

    const int av_v = avStart(sid, kVideoCh, 0);   // 直播：不可靠+FEC
    const int av_a = avStart(sid, kAudioCh, 0);
    const int rdt = RDT_Create(sid, kRdtCh);
    if (av_v < 0 || av_a < 0 || rdt < 0) {
        fprintf(stderr, "[iotc] device start ch err v=%d a=%d rdt=%d\n", av_v, av_a, rdt);
        return 1;
    }

    std::atomic<bool> run{true};
    p2p::TcpFd echo_listen;
    std::thread echo_thr;
    const uint16_t echo_port = start_tcp_echo(echo_listen, run, echo_thr);
    if (echo_port == 0) {
        fprintf(stderr, "[iotc] device tcp echo listen fail\n");
        return 1;
    }
    const int tun = P2PTunnel_Serve(sid, kTunCh, "127.0.0.1", (uint16_t)echo_port);
    if (tun < 0) {
        fprintf(stderr, "[iotc] device tunnel serve err=%d\n", tun);
        return 1;
    }
    printf("[iotc] device echo running tcp=%d tun=%d\n", echo_port, tun);
    fflush(stdout);

    std::thread t_io([av_v, &run] {
        uint8_t buf[256];
        while (run.load()) {
            uint16_t cmd = 0;
            const int n = avRecvIOCtrl(av_v, &cmd, buf, (int)sizeof(buf), 300);
            if (n < 0) continue;
            avSendIOCtrl(av_v, cmd, buf, n);
        }
    });
    std::thread t_v([av_v, &run] {
        std::vector<uint8_t> frame((size_t)kVideoLen);
        AVFrameInfo fi{};
        while (run.load()) {
            const int n = avRecvFrameData(av_v, frame.data(), kVideoLen, &fi, 300);
            if (n < 0) continue;
            avSendFrameData(av_v, frame.data(), n, &fi);
        }
    });
    std::thread t_a([av_a, &run] {
        std::vector<uint8_t> frame((size_t)kAudioLen);
        AVFrameInfo fi{};
        while (run.load()) {
            const int n = avRecvFrameData(av_a, frame.data(), kAudioLen, &fi, 300);
            if (n < 0) continue;
            avSendFrameData(av_a, frame.data(), n, &fi);
        }
    });
    std::thread t_r([rdt, &run] {
        uint8_t buf[p2p::MAX_TUNNEL_PAYLOAD];
        while (run.load()) {
            const int n = RDT_Read(rdt, buf, (int)sizeof(buf), 300);
            if (n < 0) continue;
            RDT_Write(rdt, buf, n);
        }
    });

    // 设备保持到客户端验收结束（test.sh 会杀进程）；最多 25s
    for (int i = 0; i < 50 && run.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    run = false;
    t_io.join();
    t_v.join();
    t_a.join();
    t_r.join();
    P2PTunnel_Stop(tun);
    echo_listen.shutdown_rw();
    if (echo_thr.joinable()) echo_thr.join();
    RDT_Destroy(rdt);
    avStop(av_v);
    avStop(av_a);
    IOTC_Session_Close(sid);
    return 0;
}

int run_client(const char* uid, const char* dev_uid, const char* secret, const char* key) {
    int r = IOTC_Login(uid, secret, key, 8000);
    if (r != IOTC_ER_NoERROR) {
        fprintf(stderr, "[iotc] client login err=%d\n", r);
        return 1;
    }
    printf("[iotc] client logged in uid=%s\n", uid);
    fflush(stdout);

    const int sid = IOTC_Connect_ByUID(dev_uid, 15000);
    if (sid < 0) {
        fprintf(stderr, "[iotc] client connect err=%d\n", sid);
        return 1;
    }
    IOTCSessionInfo info{};
    IOTC_Session_Check(sid, &info);
    printf("[iotc] connected sid=%d peer=%s relay=%d\n",
           sid, info.peer_uid, info.via_relay);
    fflush(stdout);
    // 给对端 Listen 返回后拉起回声线程的时间（避免首包到达时接收循环尚未启动）
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    const int av_v = avStart(sid, kVideoCh, 0);
    const int av_a = avStart(sid, kAudioCh, 0);
    const int rdt = RDT_Create(sid, kRdtCh);
    if (av_v < 0 || av_a < 0 || rdt < 0) {
        fprintf(stderr, "[iotc] client start ch err v=%d a=%d rdt=%d\n", av_v, av_a, rdt);
        return 1;
    }

    // ---- IOCtrl（短重试：ICE 刚就绪时首包可能仍在路上）----
    const char* ping = "START";
    uint16_t cmd = 0;
    char iobuf[64] = {};
    r = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        const int sr = avSendIOCtrl(av_v, kCmdPing, ping, (int)strlen(ping));
        if (sr != AV_ER_NoERROR) {
            fprintf(stderr, "[iotc] ioctl send err=%d\n", sr);
            return 1;
        }
        memset(iobuf, 0, sizeof(iobuf));
        cmd = 0;
        r = avRecvIOCtrl(av_v, &cmd, iobuf, (int)sizeof(iobuf), 3000);
        if (r == (int)strlen(ping) && cmd == kCmdPing && strcmp(iobuf, ping) == 0) break;
    }
    if (r != (int)strlen(ping) || cmd != kCmdPing || strcmp(iobuf, ping) != 0) {
        fprintf(stderr, "[iotc] ioctl echo mismatch n=%d cmd=%u\n", r, cmd);
        return 1;
    }
    printf("[iotc] ioctl echo ok\n");
    fflush(stdout);

    // ---- 视频帧（分片重组）----
    std::vector<uint8_t> vsend((size_t)kVideoLen), vrecv((size_t)kVideoLen);
    for (int i = 0; i < kVideoFrames; i++) {
        fill_pat(vsend.data(), kVideoLen, (uint8_t)(0x40 + i));
        AVFrameInfo fi{};
        fi.timestamp_ms = (uint32_t)(1000 + i * 40);
        fi.frame_type = (i == 0) ? 1 : 0;
        fi.codec_id = 1;
        r = avSendFrameData(av_v, vsend.data(), kVideoLen, &fi);
        if (r != AV_ER_NoERROR) {
            fprintf(stderr, "[iotc] video send err=%d i=%d\n", r, i);
            return 1;
        }
        AVFrameInfo fo{};
        const int n = avRecvFrameData(av_v, vrecv.data(), kVideoLen, &fo, 8000);
        if (n != kVideoLen || !same_pat(vrecv.data(), n, (uint8_t)(0x40 + i)) ||
            fo.timestamp_ms != fi.timestamp_ms || fo.frame_type != fi.frame_type) {
            fprintf(stderr, "[iotc] video echo mismatch i=%d n=%d\n", i, n);
            return 1;
        }
    }
    printf("[iotc] video echo ok frames=%d\n", kVideoFrames);
    fflush(stdout);

    // ---- 音频帧 ----
    std::vector<uint8_t> asend((size_t)kAudioLen), arecv((size_t)kAudioLen);
    for (int i = 0; i < kAudioFrames; i++) {
        fill_pat(asend.data(), kAudioLen, (uint8_t)(0x80 + i));
        AVFrameInfo fi{};
        fi.timestamp_ms = (uint32_t)(2000 + i * 20);
        fi.frame_type = 2;
        fi.codec_id = 2;
        r = avSendFrameData(av_a, asend.data(), kAudioLen, &fi);
        if (r != AV_ER_NoERROR) {
            fprintf(stderr, "[iotc] audio send err=%d i=%d\n", r, i);
            return 1;
        }
        AVFrameInfo fo{};
        const int n = avRecvFrameData(av_a, arecv.data(), kAudioLen, &fo, 5000);
        if (n != kAudioLen || !same_pat(arecv.data(), n, (uint8_t)(0x80 + i))) {
            fprintf(stderr, "[iotc] audio echo mismatch i=%d n=%d\n", i, n);
            return 1;
        }
    }
    printf("[iotc] audio echo ok frames=%d\n", kAudioFrames);
    fflush(stdout);

    // ---- RDT 字节流 ----
    std::vector<uint8_t> rsend((size_t)kRdtLen), rrecv((size_t)kRdtLen);
    fill_pat(rsend.data(), kRdtLen, 0x21);
    r = RDT_Write(rdt, rsend.data(), kRdtLen);
    if (r != RDT_ER_NoERROR) {
        fprintf(stderr, "[iotc] rdt write err=%d\n", r);
        return 1;
    }
    int got = 0;
    while (got < kRdtLen) {
        const int n = RDT_Read(rdt, rrecv.data() + got, kRdtLen - got, 8000);
        if (n < 0) {
            fprintf(stderr, "[iotc] rdt read err=%d got=%d\n", n, got);
            return 1;
        }
        got += n;
    }
    if (!same_pat(rrecv.data(), kRdtLen, 0x21)) {
        fprintf(stderr, "[iotc] rdt echo mismatch\n");
        return 1;
    }
    printf("[iotc] rdt echo ok bytes=%d\n", kRdtLen);
    fflush(stdout);

    // ---- P2PTunnel TCP 映射 ----
    const int tun = P2PTunnel_Map(sid, kTunCh, 0);
    if (tun < 0) {
        fprintf(stderr, "[iotc] tunnel map err=%d\n", tun);
        return 1;
    }
    uint16_t lport = 0;
    P2PTunnel_LocalPort(tun, &lport);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    p2p::TcpFd tc;
    if (!tc.open() || !tc.set_nodelay() || !tc.connect_to("127.0.0.1", lport)) {
        fprintf(stderr, "[iotc] tunnel tcp connect fail port=%u\n", lport);
        P2PTunnel_Stop(tun);
        return 1;
    }
    tc.set_recv_timeout_ms(3000);
    const char* hello = "tunnel-hello";
    if (tc.send_all(hello, strlen(hello)) != (ssize_t)strlen(hello)) {
        fprintf(stderr, "[iotc] tunnel send fail\n");
        P2PTunnel_Stop(tun);
        return 1;
    }
    char tbuf[64] = {};
    int tgot = 0;
    const int twant = (int)strlen(hello);
    while (tgot < twant) {
        const ssize_t n = tc.recv_some(tbuf + tgot, (size_t)(twant - tgot));
        if (n <= 0) break;
        tgot += (int)n;
    }
    if (tgot != twant || memcmp(tbuf, hello, (size_t)twant) != 0) {
        fprintf(stderr, "[iotc] tunnel echo mismatch got=%d [%s]\n", tgot, tbuf);
        P2PTunnel_Stop(tun);
        return 1;
    }
    printf("[iotc] tunnel echo ok port=%u\n", lport);
    fflush(stdout);
    P2PTunnel_Stop(tun);

    AVLinkStats st{};
    if (avGetLinkStats(av_v, &st) == AV_ER_NoERROR) {
        const int kbps = avSuggestedBitrateKbps(av_v);
        printf("[iotc] link srtt=%u cwnd=%u tx=%llu rx=%llu suggest=%d kbps\n",
               st.srtt_ms, st.cwnd, (unsigned long long)st.tx_bytes,
               (unsigned long long)st.rx_bytes, kbps);
        fflush(stdout);
    }

    printf("[iotc] 4-channel echo PASS\n");
    fflush(stdout);

    RDT_Destroy(rdt);
    avStop(av_v);
    avStop(av_a);
    IOTC_Session_Close(sid);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s device <NatIP> <NatPort> <UID> [-s secret] [-k key] [-relay ip port]\n"
                "       %s client <NatIP> <NatPort> <UID> <DevUID> [-s secret] [-k key] [-relay ip port]\n",
                argv[0], argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    const char* nat_ip = argv[2];
    const uint16_t nat_port = (uint16_t)atoi(argv[3]);
    const char* uid = argv[4];
    const char* dev_uid = nullptr;
    const char* secret = nullptr;
    const char* key = nullptr;
    const char* proxy_ip = nullptr;
    uint16_t proxy_port = 0;
    bool force_relay = false;
    int opt_start = 5;
    if (mode == "client") {
        if (argc < 6) {
            fprintf(stderr, "client needs DevUID\n");
            return 1;
        }
        dev_uid = argv[5];
        opt_start = 6;
    }
    for (int i = opt_start; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) secret = argv[++i];
        else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) key = argv[++i];
        else if (strcmp(argv[i], "-relay") == 0 && i + 2 < argc) {
            force_relay = true;
            proxy_ip = argv[++i];
            proxy_port = (uint16_t)atoi(argv[++i]);
        }
    }

    const int ir = IOTC_Initialize(nat_ip, nat_port);
    if (ir != IOTC_ER_NoERROR) {
        fprintf(stderr, "init err=%d\n", ir);
        return 1;
    }
    if (proxy_ip && proxy_port) {
        IOTC_SetProxy(proxy_ip, proxy_port);
        if (force_relay) IOTC_ForceRelay(1);
    }

    int rc = 1;
    if (mode == "device") rc = run_device(uid, secret, key);
    else if (mode == "client") rc = run_client(uid, dev_uid, secret, key);
    else {
        fprintf(stderr, "unknown mode %s\n", mode.c_str());
        rc = 1;
    }
    IOTC_DeInitialize();
    return rc;
}
