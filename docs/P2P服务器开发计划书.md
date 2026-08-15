# 类 TUTK P2P 服务器开发计划书

> 目标：基于本仓库现有代码（NatServer + P2PProxy + 客户端 SDK），演进为一套可私有化部署、
> 对标 TUTK Kalay（IOTC/AV/RDT/P2PTunnel）的 P2P 物联网连接平台，
> 首要场景为 IP 摄像机 / NVR 的远程实时视频与设备管理。
>
> 配套文档：[`代码优化分析报告.md`](../代码优化分析报告.md)（已完成的两轮优化）、
> [`流媒体优化学习路线.md`](流媒体优化学习路线.md)（传输技术学习地图）。

---

## 1. 项目定位与目标

### 1.1 对标产品

TUTK Kalay 平台的核心价值：设备烧录一个 **UID** 即可被全球任意客户端连接，
平台负责 NAT 穿透（P2P）与中继兜底（Relay），SDK 提供音视频（AV）、
可靠传输（RDT）、TCP 隧道（P2PTunnel）三类通道，穿透成功即流量走 P2P，
为流媒体节省大量服务器带宽成本。

### 1.2 本项目目标

| 维度 | 目标 |
|------|------|
| 功能 | UID 报到/连接、LAN/P2P/Relay 三模式自动选择、AV 帧级通道、RDT 可靠通道、TCP 隧道 |
| 穿透率 | P2P 直连 ≥ 85%（锥型 NAT 组合），含中继兜底整体可达率 ≥ 99.9% |
| 连接时延 | LAN < 300ms；P2P 打洞 < 3s；打洞失败切中继总时长 < 8s |
| 规模 | 单 NatServer 节点 ≥ 10 万设备在线；单 Relay 节点 ≥ 500 Mbps 转发 |
| 部署 | 全部组件可私有化部署（对标 TUTK「P2P 服务器可私有化」） |
| 安全 | 报到鉴权、端到端加密（AEAD 已有 → 前向保密握手）、防 UID 盗用/串流 |

### 1.3 非目标（本计划不含）

云存储/云录像、推送服务、账号体系 App 云（对标 Kalay 的 Cloud Recording / Push
/ Account Management 模块）——预留 API 边界，后续独立立项。

---

## 2. 术语对标：TUTK 概念 ↔ 本项目

| TUTK 概念 | 说明 | 本项目现状 | 差距 |
|-----------|------|-----------|------|
| UID（20 字节） | 平台签发的设备唯一 ID，设备以 UID 向 P2P 服务器报到 | `uuid`（≤32 字节自定义串） | 需定义 20B 结构化 UID + 签发/校验体系 |
| P2P Server | 管理 UID 报到、协助连线、全球分布 | `NatServer`（心跳注册/CONNECT 协调/跨服同步） | 集群调度/就近接入待建 |
| Relay Server | 打洞失败时转发数据 | `P2PProxy`（RELAY_DATA 转发 + HMAC 注册鉴权） | 带宽配额/计量待建 |
| IOTC Session (SID) | 设备↔客户端连接载体，上限 128 | `client/sdk/iotc/IOTC.*` SID 句柄表（上限 128） | 已落地 |
| IOTC Channel（0~31） | 会话内逻辑通道 | `IOTC_Session_Read/Write`（0~31） | 已落地 |
| AVAPIs (avIndex) | 音视频帧级传输，重传可配，上限 32 通道/连接 | `AVAPIs` + `AvCodec` 分片/重组/too-late-drop | 弱网 1080p 压测待补（P3 验收） |
| RDTAPIs | 可靠字节流 Read/Write | `RDTAPIs` 分片发送 + leftover 部分读取 | 已落地（大文件压测待补） |
| P2PTunnelAPIs | 把 TCP 协议（RTSP/HTTP/SSH）隧道化 | `P2PTunnel_Serve/Map` + `TunnelCodec` | 已落地（RTSP/ffmpeg 演示待补） |
| avSendIOCtrl | 控制指令通道 | `avSendIOCtrl/avRecvIOCtrl`（通道 0 可靠） | 已落地 |
| LAN Search | 局域网免服务器发现 | CONNECT 携带 lan 地址尝试直连 | UDP 广播发现待做 |
| Device Wakeup | 低功耗设备唤醒 | `p2p_wakeserver` + `MSG_WAKE_*` | 三平台 SDK 接入与 <6s 出图待补 |
| AuthKey / Token 鉴权 | 报到与连线鉴权 | HMAC-SHA256 挑战应答（已有） | Token 模式 + 前向保密待做 |

**结论**：连接底座（报到/打洞/中继/加密/同步）已具备且经过两轮加固，
主要差距集中在：UID 体系、SDK 通道 API 层（AV/RDT/Tunnel）、集群调度、低功耗唤醒。

---

## 3. 总体架构

```
                       ┌──────────────── 管理面 ────────────────┐
                       │  UID 签发服务   统计/计费 API   监控告警  │
                       └──────┬──────────────┬─────────────────┘
                              │              │
        ┌─────────────────────▼──────────────▼───────────────────┐
        │                    P2P 服务器集群 (NatServer)             │
        │   区域A: nat-a1 nat-a2 ←同步→ 区域B: nat-b1 nat-b2 ...    │
        │   职责: UID 报到/心跳/在线表、CONNECT 协调、NAT 探测、      │
        │        跨服注册表同步(已有)、调度返回就近 Relay             │
        └───────┬──────────────────────────────────┬─────────────┘
                │ 报到/心跳/打洞协调                  │ 可用性探测(已有)
     ┌──────────▼─────────┐                ┌────────▼───────────┐
     │  设备端 SDK          │   P2P 直连     │  Relay 集群(P2PProxy)│
     │  (IPC/NVR/嵌入式)    │◄════════════► │  打洞失败兜底转发      │
     │  UID 烧录 + 报到      │   打洞失败↘    │  注册鉴权(已有)+配额   │
     └──────────▲─────────┘      中继       └────────▲───────────┘
                │        ┌───────────────────────────┘
     ┌──────────▼────────┴─┐
     │  客户端 SDK           │  通道 API: IOTC(会话) / AV(音视频帧) /
     │  (App/PC/Web网关)     │           RDT(可靠流) / Tunnel(TCP映射)
     └─────────────────────┘
```

组件与仓库目录映射：

| 组件 | 目录 | 状态 |
|------|------|------|
| NatServer | `server/natserver/` | 已有，需扩展 UID/调度 |
| Relay | `server/proxyserver/` | 已有，需扩展配额计量 |
| 设备/客户端 SDK 核 | `client/sdk/`（api/session/transport/proto/plat） | 已有，需扩展通道 API 层 |
| 唤醒服务 | `server/wakeserver/` | 已落地（保活/触发/POKE 雏形） |
| UID 签发工具 | `tools/uidgen/` | 已落地 |
| 管理面 API | `server/natserver/src/StatusServer.cpp` 扩展 | 雏形（JSON 状态） |

---

## 4. 核心设计

### 4.1 UID 体系

对标 TUTK 20 字节 UID，采用结构化编码（Base32 大写，可读可校验）：

```
UID(20B) = PREFIX(4B) + REGION(1B) + RANDOM(12B) + CRC(3B)
  PREFIX : 客户/产品线代码（签发时分配，用于计费与隔离）
  REGION : 建议接入区域（调度提示，非强制）
  RANDOM : 密码学随机（p2p_random_bytes，已有安全熵源）
  CRC    : 前 17 字节校验（快速拒绝手输错误/爆破）
```

- **签发**：`tools/uidgen` 离线批量生成，同时产出 `UID → AuthKey(32B)` 清单；
  AuthKey 烧录进设备安全存储，服务端存 HMAC 派生值（不存明文）。
- **报到鉴权**：复用现有挑战应答（`AUTH_CHALLENGE/LOGIN`），密钥从全局
  `AuthSecret` 收敛为 **每 UID 独立 AuthKey**（防单点泄漏拖垮全网）。
- **防串流**：客户端连线携带业主签发的连线 Token（HMAC(AuthKey, uid‖有效期‖nonce)），
  NatServer 验签后才下发 CONNECT_ACK；对标 TUTK 的 `av_token` 模式。
- 兼容期：UID 与旧 uuid 并存（协议字段 33B 足够容纳 Base32 UID 字符串）。

### 4.2 会话与通道模型（对齐 IOTC/AV/RDT 三层）

```
IOTC_Session (SID)                 ← 现有 Conn+Session，补 SID 句柄表(上限128)
 ├─ Channel 0 : 默认控制通道        ← IOCtrl 指令(可靠)
 ├─ Channel 1 : AV 视频            ← 不可靠+FEC+延迟预算(帧级 API)
 ├─ Channel 2 : AV 音频            ← 不可靠+FEC(小帧高频)
 ├─ Channel 3 : RDT 文件           ← 可靠字节流(背压)
 └─ Channel N : P2PTunnel(RTSP...) ← RDT 之上的 TCP 端口映射
```

SDK API 形态（C 风格导出，贴近 TUTK 习惯便于客户迁移）：

```c
int  P2P_Initialize(const char* server_list);
int  P2P_Device_Login(const char* uid, const char* auth_key);      // 设备报到
int  P2P_Connect_ByUID(const char* uid, const char* token);        // 返回 SID
int  AV_Start(int sid, int channel, const AVConfig* cfg);          // 返回 avIndex
int  AV_SendFrame(int av, const uint8_t* frame, int len, const FrameInfo* fi);
int  AV_RecvFrame(int av, uint8_t* buf, int cap, FrameInfo* fi);   // 整帧出、可丢过期帧
int  AV_SendIOCtrl(int av, uint16_t cmd, const void* data, int len);
int  RDT_Write(int rdt, const void* data, int len);                // 可靠流，满窗阻塞/背压
int  RDT_Read(int rdt, void* buf, int cap, int timeout_ms);
int  Tunnel_Map(int sid, uint16_t local_tcp_port, uint16_t remote_tcp_port);
```

### 4.3 连接流程（三模式自动选择）

```
Client                         NatServer                       Device
  │ 1. LAN 广播探测(新增) ─────局域网────────────────────────────► │
  │    命中 → LAN 直连(免服务器)                                   │
  │ 2. CONNECT_REQ(uid,token) ──►│ 验 token/在线表                 │
  │ ◄── CONNECT_ACK(公网/私网/NAT类型/Relay候选) │── INVITE ──────► │
  │ 3. ICE 打洞（libjuice，已有）◄═══════ P2P 直连 ═══════════════► │
  │ 4. 超时/对称NAT → Relay 注册(HMAC，已有) → RELAY_DATA 中继      │
  │ 5. 中继期间打洞持续后台重试，成功即无缝升级回 P2P（新增）          │
```

模式偏好与 TUTK 一致：LAN > P2P > Relay；连接建立后 `link_stats()`（已有）
持续反馈质量，驱动通道层码率自适应与模式切换。

### 4.4 AV 通道（流媒体面，直接受益于第二轮优化）

- **帧分片/重组**：视频 I 帧远大于 `MAX_TUNNEL_PAYLOAD(1200)`，通道层按
  `frame_id + slice_idx/slice_cnt` 分片，收端整帧重组后交付（对齐 `avRecvFrameData2` 语义）。
- **重传可配**（对标 TUTK `resend` 开关）：直播模式走不可靠+FEC（已有 XOR FEC），
  回放/文件走可靠通道。
- **延迟预算 + too-late-drop**（学 SRT，第三轮传输项）：帧带时间戳，
  超预算的分片不再重传/投递，宁丢不卡。
- **丢帧策略**：拥塞（`send()` 返回 -2，已有信号）时按 非参考帧 → P 帧 → 整 GOP 丢弃，
  I 帧优先保护（FEC 加密度）。
- **音频优先**：音频通道帧小、优先级高于视频（发送队列排序）。

### 4.5 RDT 与 P2PTunnel

- RDT：在 `Session` 可靠通道（已有自适应 RTO/快速重传/拥塞窗口）之上加
  字节流化（消息边界消除）与背压（窗口满时 `RDT_Write` 阻塞或 EAGAIN）。
- P2PTunnel：本地 TCP listen ↔ RDT 通道 ↔ 对端 TCP connect 的双向搬运，
  即可透传 RTSP/ONVIF/HTTP/SSH，是存量设备零改造接入的关键卖点。

### 4.6 安全体系（在两轮加固之上）

| 层 | 现状 | 计划 |
|----|------|------|
| 报到鉴权 | 全局 secret 挑战应答、常量时间比较 | 每 UID 独立 AuthKey；失败限速与封禁（AntiAbuse 已有） |
| 连线授权 | `EnableConnectToken` + `tokengen` | 已落地（可复用至过期；nonce 防重放表未做） |
| 数据加密 | AEAD(AES-256-CTR+HMAC)、密钥自 PSK 派生 | X25519 ECDH 握手实现前向保密，AEAD 复用现有实现；预留 DTLS 选项 |
| 服务器间 | 同步 HMAC 签名+时间戳防重放（已有） | 无大改 |
| 中继 | 注册 HMAC + 源地址校验（已有） | 中继流量按 UID 计量，配额超限限速 |

### 4.7 低功耗唤醒（门铃/电池 IPC 场景）

新增 `wakeserver`：设备休眠前与唤醒服务器保持极低频 UDP 保活（NAT 映射不失效），
客户端连线时 NatServer 通知唤醒服务器发唤醒包 → 设备全速上线走正常连接流程。
协议复用 XN 报文头，新增 `MSG_WAKE_KEEPALIVE / MSG_WAKE_TRIGGER`。

---

## 5. 分阶段开发计划

> 每阶段给出范围、交付物、依赖与验收标准。阶段间尽量并行（SDK 通道层与服务端集群层独立）。

### P0 基线（已完成）
连接底座与两轮优化：打洞/中继/鉴权/加密/跨服同步/ICE/自适应 RTO/快速重传/拥塞窗口/FEC/LinkStats。
端到端测试 44 项全过（`test.sh`）。

### P1 UID 体系与设备身份【已落地（核心部分）】
- 范围：20B 结构化 UID 编解码与 CRC 校验；`tools/uidgen` 签发工具；
  每 UID AuthKey 报到鉴权（PeerManage/LicenseMgr 扩展）；连线 Token 验签；
  设备/客户端角色区分（dev_type 已有字段，语义落地）。
- 交付物：uidgen 工具、协议文档更新、服务端验签实现、单测。
- 验收：伪造 UID/过期 Token/错误 AuthKey 全部被拒；旧 uuid 兼容并存；`test.sh` 增加鉴权用例。
- **落地情况**：
  - `common/Uid.h/.cpp`：20 字符 Base32 UID（PREFIX4+REGION1+RANDOM12+CRC3，SHA-256 截断 CRC）、
    `uid_generate/uid_valid/uid_derive_auth_key`（域分隔 HMAC 派生）
  - `tools/uidgen`：批量签发，输出 `UID AuthKey_hex`；主密钥即服务端 `AuthSecret`
  - 服务端：`UidStrict=1` 时心跳/挑战双门拒绝非结构化 UID；登录验签与加密心跳流密钥
    全部改为**每 UID 派生密钥**（单设备泄露不影响全网，设备间无法互解心跳）
  - 客户端 SDK：`Config.auth_key_hex`（设备侧只烧录 AuthKey，不知晓主密钥）；
    demo `-k` 选项；旧 `-s` 主密钥模式自动派生，兼容并存
  - 验证：`tests/uid_test.cpp` 17 项断言；`test.sh` [9] 端到端（合法 UID -k 鉴权成功、
    伪造 UID 双门被拒）；全量 **PASS=50 FAIL=0**
  - 连线 Token：`common/ConnectToken.*` + `tools/tokengen`；
    `EnableConnectToken=1` 时 CONNECT 必须尾随 Token（HMAC(dst AuthKey, src‖dst‖expire‖nonce)）；
    过期/错 src/错 MAC 返回 `CONNECT_BAD_TOKEN`；旧客户端不带 Token 在开关关闭时兼容
  - 验证：`tests/token_test.cpp`；`test.sh` [14]
  - 未含：dev_type 角色语义细化

### P2 会话与通道 API 层（SDK 核心重构）【已落地】
- 范围：SID 句柄表（上限 128）与 IOTC 通道（0~31）生命周期管理；
  C 导出 API（`IOTC_Connect_ByUID`/`avStart`/...）；线程安全回调→轮询双模式。
- 依赖：无（纯 SDK 层，`P2PClient/Session` 之上封装）。
- 交付物：`client/sdk/iotc/` 新模块 + 头文件 + `iotc_demo`。
- 验收：单会话 4 通道并发（控制+视频+音频+文件）互不干扰；API 覆盖单测。
- **落地情况**：
  - `IOTC.h/.cpp`：进程级单例、SID 句柄表、每通道接收队列、阻塞 Read、
    `IOTC_Session_GetLinkStats`；锁序约定避免与 P2PClient 回调死锁
  - `AVAPIs` + `AvCodec.h`：帧分片/重组、resend 开关、too-late-drop、IOCtrl、
    `avGetLinkStats` / `avSuggestedBitrateKbps`
  - `iotc_demo`：设备/客户端四通道并发回声验收
  - 验证：`tests/av_frame_test.cpp`；`test.sh` [10] 端到端

### P3 AV 帧级通道【核心已随 P2 落地，弱网压测待补】
- 范围：帧分片/重组、重传开关、延迟预算+too-late-drop、丢帧策略、音频优先、
  IOCtrl 封装；`link_stats` 驱动的码率建议回调。
- 依赖：P2。
- 交付物：`avSendFrameData/avRecvFrameData/avSendIOCtrl` 全链路 + 弱网单测。
- 验收：`tc netem` 10% 丢包 + 100ms 延迟下，1080p 模拟流（8Mbps 帧序列）
  卡顿率 < 2%，端到端 P99 延迟 < 800ms；I 帧恢复正确。
- **已落地**：分片/重组、resend、too-late-drop、IOCtrl、码率建议。
- **本轮补齐**：`av_should_drop_p`（保护 I 帧/音频，直播拥塞丢 P）；`AV_ER_Dropped` + `avGetDropStats`；
  `av_frame_test` 1080p 量级 GOP + 10% P 切片丢失（I 帧仍重组成功）。
- **未含**：真实 `tc netem` 8Mbps 长稳压测（CI 环境无 netem 权限时用用户态切片丢失代替）。

### P4 RDT 可靠流与 P2PTunnel【核心已落地，演示压测待补】
- 范围：字节流化 + 背压；本地 TCP 端口映射隧道（RTSP 透传验证）。
- 依赖：P2。
- 交付物：`RDT_Read/Write`、`P2PTunnel_Serve/Map`；用 ffmpeg 经隧道拉取对端 RTSP 演示。
- 验收：隧道内 RTSP 播放稳定；大文件（≥1GB）经 RDT 传输校验一致；
  中继模式下同样可用。
- **落地情况**：
  - `RDTAPIs`：可靠通道分片 Write + leftover 部分 Read（窗口满走 IOTC 背压重试）
  - `TunnelCodec.h` + `P2PTunnelAPIs`：OPEN/DATA/CLOSE 帧，Serve 回连本地 TCP，
    Map 本机 listen 映射；`common/Net.h::TcpFd` RAII
  - `iotc_demo` 含 TCP echo 经隧道往返；`av_frame_test` 覆盖 TunnelCodec
- **本轮补齐**：`IOTC_SetProxy` / `IOTC_ForceRelay`；`iotc_demo -relay`；`test.sh` [11] 中继四通道。
- **未含**：ffmpeg/RTSP 演示、≥1GB 文件压测。

### P5 安全升级【核心已落地】
- 范围：X25519 ECDH 会话密钥协商（前向保密），握手消息走现有可靠通道；
  中继流量按 UID 计量与配额限速。
- 依赖：P1（UID/AuthKey 就位）。
- 交付物：握手协议文档 + 实现 + 互操作测试；Relay 计量统计接口。
- 验收：抓包验证会话密钥不可由 AuthKey 离线推导；密钥轮换不断流。
- **落地情况**：
  - `common/X25519.*`：RFC 7748 Montgomery ladder（NIST/RFC 向量单测）
  - `common/Handshake.h`：HELLO 报文 + HMAC 可选鉴权 + 会话密钥派生（PSK 不进入 FS 密钥）
  - `P2PClient`：连接建立后通道 `0xFE` 可靠握手；双密钥解密（cur/prev/PSK）支持轮换不断流；
    180s 自动换新临时密钥；`tunnel_fs_ok()` 供验收
  - `P2PProxy`：每 UID 累计转发字节 + `QuotaMB` 超限丢包（`p2p_proxy port max [workers] [QuotaMB]`）
  - 验证：`crypto_test` X25519/FS 派生；`test.sh` [5] `fs-hs=` 端到端
- **未含**：连线 Token 挂 CONNECT 门（P1 遗留）

### P6 服务端集群与调度【核心已落地，规模压测待补】
- 范围：多区域部署模型（区域内同步已有，跨区按 UID REGION 调度）；
  客户端 `GET_SERVER_LIST`（已有）扩展为就近排序；Relay 按负载/地理择优
  （`pick_proxy` 已有雏形）；Prometheus 指标导出（StatusServer 扩展）；
  中继升级回 P2P 的无缝切换。
- 依赖：P1。
- 验收：单节点 10 万模拟设备在线心跳压测 CPU < 50%；节点故障时设备
  在一个心跳周期内迁移到备节点（跨服同步保证在线表可用）。
- **落地情况**：
  - 配置：`Region` / `NatRegions` / `ProxyRegions`（`ip:R` 标注）
  - `GET_SERVER_LIST` 按请求 UID REGION（`ServerListReq`）就近稳定排序；
    无 UID 时回退本节点 `Region`
  - `pick_proxy`：健康代理空闲度加权 × 同区×3 / 本节点区×1.5；回退列表同样就近
  - StatusServer：`GET /` JSON（含 region/uptime/login_*）；`GET /metrics` Prometheus
  - 客户端：中继建链后（非 `force_relay`）后台继续打洞；`direct_ok` 后
    `via_relay` 切回 P2P 并再次回调 `on_connected(relay=false)`
  - 验证：`tests/sched_test.cpp`；`test.sh` [12]
- **未含**：10 万心跳压测、节点故障热迁移演练（跨服同步已有，本轮未扩压测）

### P7 低功耗唤醒 + 多平台 SDK【唤醒协议已落地，多平台 SDK 待补】
- 范围：wakeserver 与唤醒协议；SDK 移植层落地（`Plat.h` 已抽象）：
  Android(NDK)/iOS/Windows；嵌入式裁剪版（无 ICE 仅自研打洞，降 footprint）；
  示例 App 与集成文档。
- 依赖：P2~P4 API 冻结。
- 验收：休眠设备唤醒到出图 < 6s；三平台 demo 跑通同一 UID 互连。
- **落地情况**：
  - `server/wakeserver/`：`MSG_WAKE_KEEPALIVE / TRIGGER / RESULT / POKE`
  - 设备低频 UDP 报到（可选 `WakeSecret` HMAC）；触发后向上次公网地址发 POKE
  - NatServer `WakeServer=ip:port`：CONNECT 目标不在线时转发 TRIGGER
  - 验证：`test.sh` [13]
- **未含**：Android/iOS/Windows SDK 移植、嵌入式裁剪版、唤醒到出图 < 6s 现网验收

### 里程碑关系

```
P0(done) ─► P1 ─┬─► P5 ─► P6 ─► 现网灰度
                ├─► P2 ─► P3 ─┬─► P7 ─► GA
                │             └─► P4 ─┘
```

---

## 6. 测试与质量保障

| 层级 | 手段 | 现状/计划 |
|------|------|----------|
| 单元测试 | crypto_test、session_test（已有）→ 新增 uid/av/rdt 单测 | 每阶段验收前置 |
| 端到端 | `test.sh`（44 项，已有）持续扩展：Token 鉴权、AV 弱网、隧道 | 每次提交必跑 |
| 弱网仿真 | `tc netem`：丢包 5/10/20%、延迟 50/200/500ms、抖动、限速 | P3 起纳入 CI |
| NAT 组合矩阵 | 全锥/端口受限/对称 × 两端组合，docker + iptables 模拟 | P2 起建设 |
| 规模压测 | 模拟设备心跳器（10 万级）、Relay 吞吐打流 | P6 |
| 安全测试 | 伪造报到/重放/串流/中继盗用回归集 | P1、P5 |
| 现网灰度 | 小批量真实设备（不同运营商/地域），穿透率与连接时延埋点 | P6 后 |

## 7. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| 对称 NAT 占比高导致穿透率不达标 | 中继带宽成本上升 | 端口预测/生日攻击打洞（学 EasyTier/Tailscale）；中继常态化 + 计量控成本 |
| 嵌入式设备资源受限（RAM<1MB） | SDK 集成受阻 | 裁剪版 SDK（去 ICE/减缓冲），`Plat.h` 隔离已就绪 |
| 自研加密握手实现风险 | 安全事故 | ECDH 用成熟实现（如嵌入 tweetnacl/mbedTLS 单文件），AEAD 复用已测代码，外部评审 |
| UID 兼容迁移 | 存量设备升级困难 | 双栈并行 + 服务端灰度开关（CfgFile 热加载已有） |
| 单区域服务器故障 | 大面积掉线 | 跨服同步（已有）+ 客户端多服务器列表重连（`P2pServers.cfg` 已有） |

## 8. 交付物清单

1. 服务端：NatServer（UID/Token/调度扩展）、P2PProxy（计量配额）、wakeserver、部署手册（Docker/裸机）
2. SDK：C API 头文件 + 静态库（Linux/Android/iOS/Windows/嵌入式裁剪版）+ 集成文档
3. 工具：uidgen 签发工具、模拟设备压测器、NAT 矩阵测试环境
4. 文档：协议规范（XN/PT 帧 + 新增消息）、安全白皮书、API 参考、示例代码
5. 质量：单测 + e2e + 弱网 + 压测报告，现网灰度穿透率/时延数据
