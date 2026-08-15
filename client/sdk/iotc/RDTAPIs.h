#ifndef P2P_SDK_IOTC_RDTAPIS_H
#define P2P_SDK_IOTC_RDTAPIS_H

// RDT 可靠字节流（计划书 P4）：C 风格导出，对标 TUTK RDTAPIs
//   在 IOTC 可靠通道之上消除消息边界：Write 按包切片，Read 拼成字节流并支持部分读取。
//   窗口满时 Write 短暂重试（背压），持续满窗返回 RDT_ER_SendFail。

#include <cstdint>

extern "C" {

enum {
    RDT_MAX_CHANNELS = 128,
};

enum {
    RDT_ER_NoERROR        = 0,
    RDT_ER_InvalidArg     = -50,
    RDT_ER_ExceedMax      = -51,
    RDT_ER_ChannelNoExist = -52,
    RDT_ER_Timeout        = -53,
    RDT_ER_SendFail       = -54,
};

// 在会话 sid 的通道 channel 上开启可靠字节流；返回 rdtIndex(>=0) 或错误码
int  RDT_Create(int sid, uint8_t channel);
void RDT_Destroy(int rdt);

// 写入任意长度字节流（内部分片），返回 0 / 错误码
int  RDT_Write(int rdt, const void* data, int len);

// 读取至多 cap 字节（可能少于一次 Write 的长度），返回字节数(>0) 或错误码
int  RDT_Read(int rdt, void* buf, int cap, int timeout_ms);

} // extern "C"

#endif // P2P_SDK_IOTC_RDTAPIS_H
