// RDT 可靠字节流：IOTC 可靠通道上的分片发送 + 剩余缓冲部分读取
#include "RDTAPIs.h"

#include "IOTC.h"
#include "common/ProtoDef.h"
#include "client/sdk/plat/Plat.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr size_t kChunk = p2p::MAX_TUNNEL_PAYLOAD;

struct RdtSlot {
    bool used = false;
    int  sid = -1;
    uint8_t channel = 0;
    std::vector<uint8_t> leftover;   // 上次消息未读完的尾部
};

std::mutex g_mu;
RdtSlot g_rdt[RDT_MAX_CHANNELS];

} // namespace

extern "C" {

int RDT_Create(int sid, uint8_t channel) {
    if (sid < 0 || channel >= IOTC_MAX_CHANNELS) return RDT_ER_InvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < RDT_MAX_CHANNELS; i++) {
        if (g_rdt[i].used && g_rdt[i].sid == sid && g_rdt[i].channel == channel)
            return i;
    }
    for (int i = 0; i < RDT_MAX_CHANNELS; i++) {
        if (!g_rdt[i].used) {
            g_rdt[i] = RdtSlot{};
            g_rdt[i].used = true;
            g_rdt[i].sid = sid;
            g_rdt[i].channel = channel;
            return i;
        }
    }
    return RDT_ER_ExceedMax;
}

void RDT_Destroy(int rdt) {
    if (rdt < 0 || rdt >= RDT_MAX_CHANNELS) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_rdt[rdt] = RdtSlot{};
}

int RDT_Write(int rdt, const void* data, int len) {
    if (!data || len <= 0) return RDT_ER_InvalidArg;
    int sid;
    uint8_t channel;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (rdt < 0 || rdt >= RDT_MAX_CHANNELS || !g_rdt[rdt].used)
            return RDT_ER_ChannelNoExist;
        sid = g_rdt[rdt].sid;
        channel = g_rdt[rdt].channel;
    }
    const uint8_t* p = (const uint8_t*)data;
    int off = 0;
    while (off < len) {
        const int n = ((len - off) < (int)kChunk) ? (len - off) : (int)kChunk;
        const int r = IOTC_Session_Write(sid, channel, p + off, n, 1);
        if (r != IOTC_ER_NoERROR) return RDT_ER_SendFail;
        off += n;
    }
    return RDT_ER_NoERROR;
}

int RDT_Read(int rdt, void* buf, int cap, int timeout_ms) {
    if (!buf || cap <= 0) return RDT_ER_InvalidArg;
    int sid;
    uint8_t channel;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (rdt < 0 || rdt >= RDT_MAX_CHANNELS || !g_rdt[rdt].used)
            return RDT_ER_ChannelNoExist;
        if (!g_rdt[rdt].leftover.empty()) {
            const int n = ((int)g_rdt[rdt].leftover.size() < cap)
                              ? (int)g_rdt[rdt].leftover.size() : cap;
            memcpy(buf, g_rdt[rdt].leftover.data(), (size_t)n);
            g_rdt[rdt].leftover.erase(g_rdt[rdt].leftover.begin(),
                                      g_rdt[rdt].leftover.begin() + n);
            return n;
        }
        sid = g_rdt[rdt].sid;
        channel = g_rdt[rdt].channel;
    }

    uint8_t msg[p2p::MAX_TUNNEL_PAYLOAD];
    const int n = IOTC_Session_Read(sid, channel, msg, sizeof(msg), timeout_ms);
    if (n == IOTC_ER_Timeout) return RDT_ER_Timeout;
    if (n <= 0) return RDT_ER_ChannelNoExist;

    const int take = (n < cap) ? n : cap;
    memcpy(buf, msg, (size_t)take);
    if (n > take) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_rdt[rdt].used) {
            g_rdt[rdt].leftover.assign(msg + take, msg + n);
        }
    }
    return take;
}

} // extern "C"
