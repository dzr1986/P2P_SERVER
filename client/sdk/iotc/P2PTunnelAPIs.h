#ifndef P2P_SDK_IOTC_P2PTUNNEL_APIS_H
#define P2P_SDK_IOTC_P2PTUNNEL_APIS_H

// P2PTunnel TCP 端口映射（计划书 P4）：C 风格导出，对标 TUTK P2PTunnelAPIs
//   设备端 Serve：对端 OPEN 时回连本地 target（如 RTSP/HTTP）
//   客户端 Map  ：本机 listen，accept 后经 IOTC 可靠通道搬到设备
//   通道内帧格式见 TunnelCodec.h

#include <cstdint>

extern "C" {

enum {
    TUNNEL_MAX_HANDLES = 32,
};

enum {
    TUNNEL_ER_NoERROR        = 0,
    TUNNEL_ER_InvalidArg     = -70,
    TUNNEL_ER_ExceedMax      = -71,
    TUNNEL_ER_HandleNoExist  = -72,
    TUNNEL_ER_ListenFail     = -73,
    TUNNEL_ER_ConnectFail    = -74,
};

// 设备端：在 sid/channel 上等待对端 OPEN，回连 target_ip:target_port
// 返回 handle(>=0) 或错误码
int  P2PTunnel_Serve(int sid, uint8_t channel,
                     const char* target_ip, uint16_t target_port);

// 客户端：本机 127.0.0.1:local_port 监听（0=系统分配），映射到对端 Serve
int  P2PTunnel_Map(int sid, uint8_t channel, uint16_t local_port);

// 查询 Map 实际监听端口（local_port=0 时用）
int  P2PTunnel_LocalPort(int handle, uint16_t* port);

void P2PTunnel_Stop(int handle);

} // extern "C"

#endif // P2P_SDK_IOTC_P2PTUNNEL_APIS_H
