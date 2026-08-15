#ifndef P2P_SDK_IOTC_AVAPIS_H
#define P2P_SDK_IOTC_AVAPIS_H

// AV 通道层（计划书 P2）：C 风格导出，对标 TUTK AVAPIs 使用习惯
//   - 基于 IOTC 会话通道的帧级传输：整帧进出（内部自动分片/重组）
//   - resend 可配：1=可靠（回放/文件），0=不可靠+FEC（直播低延迟，
//     新帧完成即丢弃更旧未完成帧，宁丢不卡）
//   - IOCtrl 控制指令走会话通道 0（可靠），与音视频通道互不干扰

#include <cstdint>

extern "C" {

enum {
    AV_MAX_CHANNELS_TOTAL = 128,   // 进程级 avIndex 句柄上限
};

enum {
    AV_ER_NoERROR        = 0,
    AV_ER_InvalidArg     = -30,
    AV_ER_ExceedMax      = -31,
    AV_ER_ChannelNoExist = -32,
    AV_ER_Timeout        = -33,
    AV_ER_BufferTooSmall = -34,
    AV_ER_SendFail       = -35,
    AV_ER_FrameTooLarge  = -36,
    AV_ER_Dropped        = -37,   // 拥塞主动丢 P 帧（直播宁丢不卡）
};

// 帧元信息（与 AvCodec.h AvFrameInfo 二进制一致）
typedef struct {
    uint32_t timestamp_ms;
    uint8_t  frame_type;   // 0=P 1=I 2=音频（应用自定义）
    uint8_t  codec_id;
} AVFrameInfo;

// 在会话 sid 的通道 channel(1~31，0 保留给 IOCtrl) 上开启 AV 传输
//   resend: 1=可靠 0=不可靠+FEC
//   返回 avIndex(>=0) 或错误码；设备端与客户端两侧均需调用
int avStart(int sid, uint8_t channel, int resend);
void avStop(int av);

// 发送整帧（内部分片），返回 0 / 错误码
int avSendFrameData(int av, const void* frame, int len, const AVFrameInfo* fi);

// 接收整帧（内部重组，阻塞至多 timeout_ms），返回帧字节数(>0) 或错误码
int avRecvFrameData(int av, void* buf, int cap, AVFrameInfo* fi, int timeout_ms);

// IOCtrl 控制指令（会话通道 0，可靠）：cmd 应用自定义
int avSendIOCtrl(int av, uint16_t cmd, const void* data, int len);
// 返回负载字节数(>=0) 并填 cmd，或错误码
int avRecvIOCtrl(int av, uint16_t* cmd, void* buf, int cap, int timeout_ms);

// 链路统计 / 码率建议（P3：link_stats 驱动自适应码率）
typedef struct {
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    uint32_t rto_ms;
    uint32_t cwnd;
    uint32_t inflight;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t retrans;
    uint64_t fec_recovered;
    uint64_t rx_lost;
    uint32_t twcc_kbps;
} AVLinkStats;
int avGetLinkStats(int av, AVLinkStats* st);
// 基于 RTT/窗口/丢包给出建议码率（kbps，AIMD 平滑）；失败返回 <0
int avSuggestedBitrateKbps(int av);

// 丢帧统计：dropped_p=拥塞丢弃的 P 帧数，sent=成功发出的帧数
int avGetDropStats(int av, uint64_t* dropped_p, uint64_t* sent);

} // extern "C"

#endif // P2P_SDK_IOTC_AVAPIS_H
