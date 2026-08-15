// av_frame_test：AV 分片/重组 + P2PTunnel 帧编解码单测（计划书 P2/P4）
#include "client/sdk/iotc/AvCodec.h"
#include "client/sdk/iotc/TunnelCodec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace p2p;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

int main() {
    // ---- AvCodec：单切片往返 ----
    const char* hello = "hello-av";
    AvFrameInfo fi{};
    fi.timestamp_ms = 0x11223344;
    fi.frame_type = 1;
    fi.codec_id = 7;
    uint8_t slice[MAX_TUNNEL_PAYLOAD];
    const size_t n1 = av_write_slice(slice, sizeof(slice), 1, 0, 1, fi,
                                     (const uint8_t*)hello, strlen(hello));
    CHECK(n1 == AV_SLICE_HDR + strlen(hello), "single slice write size");

    uint16_t fid = 0;
    uint8_t idx = 0, cnt = 0;
    AvFrameInfo fo{};
    const uint8_t* payload = nullptr;
    size_t plen = 0;
    CHECK(av_read_slice(slice, n1, fid, idx, cnt, fo, payload, plen), "single slice parse");
    CHECK(fid == 1 && idx == 0 && cnt == 1, "single slice ids");
    CHECK(fo.timestamp_ms == 0x11223344 && fo.frame_type == 1 && fo.codec_id == 7,
          "single slice meta");
    CHECK(plen == strlen(hello) && memcmp(payload, hello, plen) == 0, "single slice payload");

    // ---- 多切片重组 ----
    const size_t big_len = AV_SLICE_PAYLOAD * 3 + 17;
    std::vector<uint8_t> big(big_len);
    for (size_t i = 0; i < big_len; i++) big[i] = (uint8_t)(i * 3);
    CHECK(av_slice_count(big_len) == 4, "slice count for 3*payload+17");

    AvReassembler rx;
    std::vector<uint8_t> out;
    AvFrameInfo fr{};
    bool done = false;
    const uint8_t scnt = (uint8_t)av_slice_count(big_len);
    for (uint8_t i = 0; i < scnt; i++) {
        const size_t off = (size_t)i * AV_SLICE_PAYLOAD;
        const size_t chunk = (big_len - off) < AV_SLICE_PAYLOAD ? (big_len - off)
                                                                : AV_SLICE_PAYLOAD;
        const size_t wn = av_write_slice(slice, sizeof(slice), 9, i, scnt, fi,
                                         big.data() + off, chunk);
        CHECK(wn > 0, "multi slice write");
        done = rx.feed(slice, wn, out, fr);
        if (i + 1 < scnt) CHECK(!done, "incomplete until last slice");
    }
    CHECK(done, "reassembler completes on last slice");
    CHECK(out.size() == big_len && out == big, "reassembled payload matches");
    CHECK(fr.timestamp_ms == fi.timestamp_ms && fr.frame_type == 1, "reassembled meta");
    CHECK(rx.pending_frames() == 0, "no pending after complete");

    // ---- 乱序切片 ----
    AvReassembler rx2;
    std::vector<uint8_t> parts[2];
    const char* a = "AAAA";
    const char* b = "BBBB";
    parts[0].resize(64);
    parts[1].resize(64);
    av_write_slice(parts[0].data(), parts[0].size(), 3, 0, 2, fi, (const uint8_t*)a, 4);
    av_write_slice(parts[1].data(), parts[1].size(), 3, 1, 2, fi, (const uint8_t*)b, 4);
    CHECK(!rx2.feed(parts[1].data(), AV_SLICE_HDR + 4, out, fr), "out-of-order first (idx=1)");
    CHECK(rx2.feed(parts[0].data(), AV_SLICE_HDR + 4, out, fr), "out-of-order completes");
    CHECK(out.size() == 8 && memcmp(out.data(), "AAAABBBB", 8) == 0, "out-of-order payload");

    // ---- too-late-drop：完成新帧后丢弃更旧未完成帧 ----
    AvReassembler rx3;
    uint8_t olds[64], news[64];
    av_write_slice(olds, sizeof(olds), 10, 0, 2, fi, (const uint8_t*)"X", 1);
    CHECK(!rx3.feed(olds, AV_SLICE_HDR + 1, out, fr), "old partial stays pending");
    CHECK(rx3.pending_frames() == 1, "one pending old frame");
    av_write_slice(news, sizeof(news), 12, 0, 1, fi, (const uint8_t*)"Y", 1);
    CHECK(rx3.feed(news, AV_SLICE_HDR + 1, out, fr), "newer single-slice completes");
    CHECK(rx3.pending_frames() == 0, "older partial dropped");
    CHECK(rx3.dropped_frames() >= 1, "dropped counter incremented");

    // ---- 非法输入 ----
    CHECK(!av_read_slice(slice, 3, fid, idx, cnt, fo, payload, plen), "short buffer rejected");
    slice[0] = 0x00;
    CHECK(!av_read_slice(slice, n1, fid, idx, cnt, fo, payload, plen), "bad magic rejected");
    CHECK(av_write_slice(slice, 4, 1, 0, 1, fi, (const uint8_t*)hello, strlen(hello)) == 0,
          "tiny cap rejected");
    CHECK(av_slice_count(0) == 0, "zero-length frame is 0 slices");

    // ---- TunnelCodec 往返 ----
    uint8_t tun[128];
    const char* tpay = "open-rtsp";
    const size_t tn = tun_write(tun, sizeof(tun), TUN_OPEN, 0x1234,
                                (const uint8_t*)tpay, strlen(tpay));
    CHECK(tn == TUN_HDR + strlen(tpay), "tun write size");
    uint8_t ttype = 0;
    uint16_t cid = 0;
    const uint8_t* tp = nullptr;
    size_t tlen = 0;
    CHECK(tun_read(tun, tn, ttype, cid, tp, tlen), "tun parse");
    CHECK(ttype == TUN_OPEN && cid == 0x1234, "tun ids");
    CHECK(tlen == strlen(tpay) && memcmp(tp, tpay, tlen) == 0, "tun payload");

    const size_t tn0 = tun_write(tun, sizeof(tun), TUN_CLOSE, 7, nullptr, 0);
    CHECK(tn0 == TUN_HDR && tun_read(tun, tn0, ttype, cid, tp, tlen), "tun close empty");
    CHECK(ttype == TUN_CLOSE && cid == 7 && tlen == 0, "tun close fields");

    CHECK(tun_write(tun, 3, TUN_DATA, 1, (const uint8_t*)"x", 1) == 0, "tun tiny cap rejected");
    tun[0] = 0x00;
    CHECK(!tun_read(tun, tn0, ttype, cid, tp, tlen), "tun bad magic rejected");
    CHECK(!tun_read(tun, 2, ttype, cid, tp, tlen), "tun short rejected");

    if (g_fail == 0) {
        printf("av/tunnel codec tests PASS\n");
        return 0;
    }
    printf("av/tunnel codec tests FAIL (%d)\n", g_fail);
    return 1;
}
