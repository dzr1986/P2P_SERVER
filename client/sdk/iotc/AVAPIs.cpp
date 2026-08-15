// AV 通道层实现（计划书 P2）：帧分片/重组 + IOCtrl，基于 IOTC 会话通道
#include "AVAPIs.h"

#include "AvCodec.h"
#include "IOTC.h"
#include "client/sdk/plat/Plat.h"
#include "common/AbrEstimate.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace {

using namespace p2p;

// IOCtrl 帧布局（通道 0，可靠）：magic(1)=0xCC | cmd(2 BE) | payload
constexpr uint8_t  kIoctlMagic = 0xCC;
constexpr size_t   kIoctlHdr = 3;
constexpr uint8_t  kIoctlChannel = 0;

struct AvChannel {
    bool used = false;
    int  sid = -1;
    uint8_t channel = 0;
    bool resend = false;
    uint16_t tx_frame_id = 0;
    AvReassembler rx;
    bool last_fail = false;
    uint64_t dropped_p = 0;
    uint64_t sent_frames = 0;
    AbrController abr;
};

std::mutex g_mu;
AvChannel g_ch[AV_MAX_CHANNELS_TOTAL];

} // namespace

extern "C" {

int avStart(int sid, uint8_t channel, int resend) {
    if (sid < 0 || channel == kIoctlChannel || channel >= IOTC_MAX_CHANNELS)
        return AV_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    // 同一 (sid, channel) 幂等复用
    for (int i = 0; i < AV_MAX_CHANNELS_TOTAL; i++) {
        if (g_ch[i].used && g_ch[i].sid == sid && g_ch[i].channel == channel)
            return i;
    }
    for (int i = 0; i < AV_MAX_CHANNELS_TOTAL; i++) {
        if (!g_ch[i].used) {
            g_ch[i] = AvChannel{};
            g_ch[i].used = true;
            g_ch[i].sid = sid;
            g_ch[i].channel = channel;
            g_ch[i].resend = (resend != 0);
            return i;
        }
    }
    return AV_ER_ExceedMax;
}

void avStop(int av) {
    if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_ch[av] = AvChannel{};
}

int avSendFrameData(int av, const void* frame, int len, const AVFrameInfo* fi) {
    if (!frame || len <= 0 || !fi) return AV_ER_InvalidArg;
    if ((size_t)len > AV_MAX_FRAME) return AV_ER_FrameTooLarge;

    int sid;
    uint8_t channel;
    bool resend;
    uint16_t fid;
    bool last_fail = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
            return AV_ER_ChannelNoExist;
        sid = g_ch[av].sid;
        channel = g_ch[av].channel;
        resend = g_ch[av].resend;
        last_fail = g_ch[av].last_fail;
        fid = g_ch[av].tx_frame_id++;
    }

    bool congested = last_fail;
    IOTCLinkStats ls{};
    if (IOTC_Session_GetLinkStats(sid, &ls) == IOTC_ER_NoERROR) {
        if (ls.cwnd > 0 && ls.inflight >= ls.cwnd) congested = true;
        if (ls.srtt_ms > 400) congested = true;
        if (ls.rx_lost > 8) congested = true;
    }
    if (av_should_drop_p(fi->frame_type, resend, congested)) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ch[av].used) g_ch[av].dropped_p++;
        return AV_ER_Dropped;
    }

    AvFrameInfo afi;
    afi.timestamp_ms = fi->timestamp_ms;
    afi.frame_type = fi->frame_type;
    afi.codec_id = fi->codec_id;

    const uint8_t* p = (const uint8_t*)frame;
    const size_t cnt = av_slice_count((size_t)len);
    uint8_t slice[MAX_TUNNEL_PAYLOAD];
    for (size_t i = 0; i < cnt; i++) {
        const size_t off = i * AV_SLICE_PAYLOAD;
        const size_t plen = ((size_t)len - off) < AV_SLICE_PAYLOAD
                                ? (size_t)len - off : AV_SLICE_PAYLOAD;
        const size_t n = av_write_slice(slice, sizeof(slice), fid, (uint8_t)i,
                                        (uint8_t)cnt, afi, p + off, plen);
        if (n == 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_ch[av].used) g_ch[av].last_fail = true;
            return AV_ER_SendFail;
        }
        const int r = IOTC_Session_Write(sid, channel, slice, (int)n, resend ? 1 : 0);
        if (r != IOTC_ER_NoERROR) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_ch[av].used) g_ch[av].last_fail = true;
            return AV_ER_SendFail;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ch[av].used) {
            g_ch[av].last_fail = false;
            g_ch[av].sent_frames++;
        }
    }
    return AV_ER_NoERROR;
}

int avRecvFrameData(int av, void* buf, int cap, AVFrameInfo* fi, int timeout_ms) {
    if (!buf || cap <= 0 || !fi) return AV_ER_InvalidArg;
    int sid;
    uint8_t channel;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
            return AV_ER_ChannelNoExist;
        sid = g_ch[av].sid;
        channel = g_ch[av].channel;
    }

    const uint64_t deadline = plat_now_ms() + (uint64_t)timeout_ms;
    uint8_t slice[MAX_TUNNEL_PAYLOAD];
    std::vector<uint8_t> out;
    AvFrameInfo afi;
    for (;;) {
        const uint64_t now = plat_now_ms();
        if (now >= deadline) return AV_ER_Timeout;
        const int n = IOTC_Session_Read(sid, channel, slice, sizeof(slice),
                                        (int)(deadline - now));
        if (n == IOTC_ER_Timeout) return AV_ER_Timeout;
        if (n <= 0) return AV_ER_ChannelNoExist;

        bool complete;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_ch[av].used) return AV_ER_ChannelNoExist;
            complete = g_ch[av].rx.feed(slice, (size_t)n, out, afi);
        }
        if (!complete) continue;
        if ((int)out.size() > cap) return AV_ER_BufferTooSmall;
        memcpy(buf, out.data(), out.size());
        fi->timestamp_ms = afi.timestamp_ms;
        fi->frame_type = afi.frame_type;
        fi->codec_id = afi.codec_id;
        return (int)out.size();
    }
}

int avSendIOCtrl(int av, uint16_t cmd, const void* data, int len) {
    if (len < 0 || (len > 0 && !data)) return AV_ER_InvalidArg;
    int sid;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
            return AV_ER_ChannelNoExist;
        sid = g_ch[av].sid;
    }
    if (kIoctlHdr + (size_t)len > MAX_TUNNEL_PAYLOAD) return AV_ER_InvalidArg;
    uint8_t buf[MAX_TUNNEL_PAYLOAD];
    buf[0] = kIoctlMagic;
    buf[1] = (uint8_t)(cmd >> 8);
    buf[2] = (uint8_t)(cmd & 0xFF);
    if (len > 0) memcpy(buf + kIoctlHdr, data, (size_t)len);
    const int r = IOTC_Session_Write(sid, kIoctlChannel, buf,
                                     (int)(kIoctlHdr + (size_t)len), 1);
    return r == IOTC_ER_NoERROR ? AV_ER_NoERROR : AV_ER_SendFail;
}

int avRecvIOCtrl(int av, uint16_t* cmd, void* buf, int cap, int timeout_ms) {
    if (!cmd || (!buf && cap > 0) || cap < 0) return AV_ER_InvalidArg;
    int sid;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
            return AV_ER_ChannelNoExist;
        sid = g_ch[av].sid;
    }
    const uint64_t deadline = plat_now_ms() + (uint64_t)timeout_ms;
    uint8_t msg[MAX_TUNNEL_PAYLOAD];
    for (;;) {
        const uint64_t now = plat_now_ms();
        if (now >= deadline) return AV_ER_Timeout;
        const int n = IOTC_Session_Read(sid, kIoctlChannel, msg, sizeof(msg),
                                        (int)(deadline - now));
        if (n == IOTC_ER_Timeout) return AV_ER_Timeout;
        if (n <= 0) return AV_ER_ChannelNoExist;
        if ((size_t)n < kIoctlHdr || msg[0] != kIoctlMagic) continue;   // 非 IOCtrl 忽略
        *cmd = (uint16_t)((msg[1] << 8) | msg[2]);
        const int plen = n - (int)kIoctlHdr;
        if (plen > cap) return AV_ER_BufferTooSmall;
        if (plen > 0) memcpy(buf, msg + kIoctlHdr, (size_t)plen);
        return plen;
    }
}

int avGetLinkStats(int av, AVLinkStats* st) {
    if (!st) return AV_ER_InvalidArg;
    int sid;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
            return AV_ER_ChannelNoExist;
        sid = g_ch[av].sid;
    }
    IOTCLinkStats ls;
    const int r = IOTC_Session_GetLinkStats(sid, &ls);
    if (r != IOTC_ER_NoERROR) return AV_ER_ChannelNoExist;
    st->srtt_ms = ls.srtt_ms;
    st->rttvar_ms = ls.rttvar_ms;
    st->rto_ms = ls.rto_ms;
    st->cwnd = ls.cwnd;
    st->inflight = ls.inflight;
    st->tx_bytes = ls.tx_bytes;
    st->rx_bytes = ls.rx_bytes;
    st->retrans = ls.retrans;
    st->fec_recovered = ls.fec_recovered;
    st->rx_lost = ls.rx_lost;
    st->twcc_kbps = ls.twcc_kbps;
    return AV_ER_NoERROR;
}

int avGetDropStats(int av, uint64_t* dropped_p, uint64_t* sent) {
    if (!dropped_p || !sent) return AV_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
        return AV_ER_ChannelNoExist;
    *dropped_p = g_ch[av].dropped_p;
    *sent = g_ch[av].sent_frames;
    return AV_ER_NoERROR;
}

int avSuggestedBitrateKbps(int av) {
    AVLinkStats st;
    if (avGetLinkStats(av, &st) != AV_ER_NoERROR) return AV_ER_ChannelNoExist;
    std::lock_guard<std::mutex> lk(g_mu);
    if (av < 0 || av >= AV_MAX_CHANNELS_TOTAL || !g_ch[av].used)
        return AV_ER_ChannelNoExist;
    return g_ch[av].abr.update(st.srtt_ms, st.rttvar_ms, st.cwnd, st.rx_lost,
                               (int)st.twcc_kbps);
}

} // extern "C"
