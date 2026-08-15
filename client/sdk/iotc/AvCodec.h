#ifndef P2P_SDK_IOTC_AV_CODEC_H
#define P2P_SDK_IOTC_AV_CODEC_H

// AV 帧分片编解码（计划书 P2，头文件实现便于单测）
//   音视频帧常大于单包上限（MAX_TUNNEL_PAYLOAD=1200），通道层按帧切片：
//   切片布局（负载内，11 字节头）：
//     magic(1)=0xAF | frame_id(2 BE) | slice_idx(1) | slice_cnt(1)
//     | frame_type(1) | codec_id(1) | timestamp_ms(4 BE)
//   接收端按 frame_id 重组；新帧完成时丢弃更旧的未完成帧（too-late-drop，
//   直播宁丢不卡；可靠模式下不会触发丢弃）。

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "common/ProtoDef.h"

namespace p2p {

constexpr uint8_t AV_SLICE_MAGIC = 0xAF;
constexpr size_t  AV_SLICE_HDR = 11;
constexpr size_t  AV_SLICE_PAYLOAD = MAX_TUNNEL_PAYLOAD - AV_SLICE_HDR;
constexpr int     AV_MAX_SLICES = 255;
constexpr size_t  AV_MAX_FRAME = AV_SLICE_PAYLOAD * AV_MAX_SLICES;

// 帧元信息（对标 TUTK FRAMEINFO 精简版）
struct AvFrameInfo {
    uint32_t timestamp_ms = 0;
    uint8_t  frame_type = 0;   // 0=P/普通 1=I 帧 2=音频（应用自定义）
    uint8_t  codec_id = 0;     // 应用自定义（如 H264/H265/OPUS 编号）
};

inline size_t av_slice_count(size_t frame_len) {
    return (frame_len + AV_SLICE_PAYLOAD - 1) / AV_SLICE_PAYLOAD;
}

// 构造一个切片，返回写入字节数；容量不足返回 0
inline size_t av_write_slice(uint8_t* out, size_t cap, uint16_t frame_id,
                             uint8_t idx, uint8_t cnt, const AvFrameInfo& fi,
                             const uint8_t* payload, size_t plen) {
    if (cap < AV_SLICE_HDR + plen || plen > AV_SLICE_PAYLOAD) return 0;
    out[0] = AV_SLICE_MAGIC;
    out[1] = (uint8_t)(frame_id >> 8);
    out[2] = (uint8_t)(frame_id & 0xFF);
    out[3] = idx;
    out[4] = cnt;
    out[5] = fi.frame_type;
    out[6] = fi.codec_id;
    out[7] = (uint8_t)(fi.timestamp_ms >> 24);
    out[8] = (uint8_t)(fi.timestamp_ms >> 16);
    out[9] = (uint8_t)(fi.timestamp_ms >> 8);
    out[10] = (uint8_t)(fi.timestamp_ms & 0xFF);
    memcpy(out + AV_SLICE_HDR, payload, plen);
    return AV_SLICE_HDR + plen;
}

// 解析切片头，payload 指向切片内数据；非法返回 false
inline bool av_read_slice(const uint8_t* buf, size_t len, uint16_t& frame_id,
                          uint8_t& idx, uint8_t& cnt, AvFrameInfo& fi,
                          const uint8_t*& payload, size_t& plen) {
    if (len < AV_SLICE_HDR || buf[0] != AV_SLICE_MAGIC) return false;
    frame_id = (uint16_t)((buf[1] << 8) | buf[2]);
    idx = buf[3];
    cnt = buf[4];
    if (cnt == 0 || idx >= cnt) return false;
    fi.frame_type = buf[5];
    fi.codec_id = buf[6];
    fi.timestamp_ms = ((uint32_t)buf[7] << 24) | ((uint32_t)buf[8] << 16) |
                      ((uint32_t)buf[9] << 8) | buf[10];
    payload = buf + AV_SLICE_HDR;
    plen = len - AV_SLICE_HDR;
    return true;
}

// 接收端重组器：feed 切片，整帧完成返回 true 并填充 out/fi
class AvReassembler {
public:
    bool feed(const uint8_t* buf, size_t len,
              std::vector<uint8_t>& out, AvFrameInfo& fi_out) {
        uint16_t fid;
        uint8_t idx, cnt;
        AvFrameInfo fi;
        const uint8_t* payload;
        size_t plen;
        if (!av_read_slice(buf, len, fid, idx, cnt, fi, payload, plen)) return false;

        if (partial_.find(fid) == partial_.end() && partial_.size() >= kMaxPartial) {
            partial_.erase(partial_.begin());   // 安全阀：未完成帧过多则丢最旧
            dropped_++;
        }
        auto& p = partial_[fid];
        if (p.slices.empty()) {
            p.slices.resize(cnt);
            p.got = 0;
            p.cnt = cnt;
            p.fi = fi;
        }
        if (cnt != p.cnt || idx >= p.slices.size()) {   // 帧参数不一致：重置该帧
            partial_.erase(fid);
            return false;
        }
        if (p.slices[idx].empty()) {
            p.slices[idx].assign(payload, payload + plen);
            p.got++;
        }
        if (p.got < p.cnt) return false;

        // 整帧完成：拼接输出
        out.clear();
        for (auto& s : p.slices) out.insert(out.end(), s.begin(), s.end());
        fi_out = p.fi;
        const uint16_t done = fid;
        partial_.erase(done);
        drop_older_than(done);   // too-late-drop：比完成帧更旧的未完成帧不再等待
        return true;
    }

    size_t pending_frames() const { return partial_.size(); }
    uint64_t dropped_frames() const { return dropped_; }

private:
    struct Partial {
        std::vector<std::vector<uint8_t>> slices;
        uint8_t got = 0;
        uint8_t cnt = 0;
        AvFrameInfo fi;
    };

    void drop_older_than(uint16_t fid) {
        for (auto it = partial_.begin(); it != partial_.end();) {
            if ((int16_t)(it->first - fid) < 0) {
                it = partial_.erase(it);
                dropped_++;
            } else {
                ++it;
            }
        }
    }

    static constexpr size_t kMaxPartial = 16;

    std::map<uint16_t, Partial> partial_;
    uint64_t dropped_ = 0;
};

} // namespace p2p

#endif // P2P_SDK_IOTC_AV_CODEC_H
