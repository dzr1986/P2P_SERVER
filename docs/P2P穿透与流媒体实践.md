# P2P 穿透与流媒体实践（对照本仓库）

> 更新：2026-08-16。检索基线：RFC 8445/8489/8656/8838、WebRTC 生产经验（2025–2026）、
> TUTK Kalay 开发文档、Tailscale DERP 编排、SRT too-late-drop。
> 本文回答：**P2P 一对一预览有哪些不可绕过的特点**，以及本仓库怎么对齐。
>
> 模型取舍（NAT 二维、DCUtR、WHIP/MoQ）见 [`P2P模型图谱.md`](P2P模型图谱.md)；
> 阶段见 [`P2P服务器开发计划书.md`](P2P服务器开发计划书.md)（档 A 已收口 / 档 B 现网 / 档 C 旁路）。

---

## 0. 怎么用本文

| 你想… | 看 |
|--------|----|
| P2P 预览和推云直播差在哪 | §1 |
| ICE / STUN / 中继怎么落到代码 | §2 |
| 打洞成功后帧怎么走 | §3 |
| 同网段为什么必须先 LAN | §4 |
| 现网还缺什么观测 | §5 |
| 档 B / 档 C 缺口 | §6 |

---

## 1. P2P 流媒体的产品特点

家用/运营商 NAT 默认**只允许内网主动发出的五元组回流**。摄像机在 NAT 后监听
UDP/TCP，公网 App 无法直接连入。行业只有两条路：

| 路径 | 适用 | 代价 |
|------|------|------|
| 设备推流到云（RTMP/SRT → HLS/WebRTC 分发） | 一对多直播、回看 | 带宽始终走云 |
| **P2P 打洞 + 中继兜底**（TUTK / 本仓库） | 一对一预览、对讲 | 直连成功后媒体不经云；失败才中继 |

一对一 IPC 预览必须走第二条：NatServer 只做**信令与地址交换**，媒体尽量直连。
一对多仍应走 CDN / SFU，不要用设备侧 mesh。下面是 2025–2026 生产里反复出现、
且对本仓库有效的特点。

### 1.1 三条路径是质量阶梯，不是等价备份

TUTK 把会话分成 **LAN / P2P / RLY**。Bambu 等接入方实测也一样：同网 JPEG/RTSP
画质和时延明显好于远程 TUTK。本仓库模式仍是 **LAN > P2P > Relay**：

| 模式 | 带宽/时延 | 码率建议 |
|------|-----------|----------|
| LAN | 稳、几乎无中继税 | 可 1080p 满码 |
| P2P 直连 | 取决于两端家宽/4G | ABR 按 TWCC |
| Relay（RLY） | 多一跳 RTT + 服务器配额 | **主动降码**；容量按 15–30% 会话估 |

`force_relay` / `peer -relay` 对标 WebRTC `iceTransportPolicy: "relay"`：
**硬关打洞**，测试 [2]/[11]/[21] 依赖，NEED 也不能打开。

### 1.2 先通再优，不要先打洞再黑屏

经典 ICE：并行收 host/srflx/relay，提名后再出图。企业拦 UDP 时，用户会在
ICE 完成前一直黑屏。

Tailscale DERP 的编排（只学编排，不抄 VPN）：**先走 TCP/TLS 443 中继出图，
后台打洞成功再无缝切直连**。Trickle ICE（RFC 8838）同理：host 候选几十毫秒内
就能开始检查，不必等 TURN Allocate（常 100–500ms）。公开对比：相对整包 SDP，
Trickle 可把 **Time-to-First-Frame 缩短 200–800ms**，中继重路径上可到 2–4s。

本仓库：CONNECT 后可立刻建 DERP TCP/TLS；ICE 并行；`direct_ok` 后
`PathSelect` 改走 juice，不再占中继。

### 1.3 中继是一等公民

WebRTC 生产数字（2025–2026，多源交叉）：**约 15–30% 会话必须中继**
（对称 NAT、企业防火墙、CGNAT、酒店只放 443）。没有 Relay = 放弃这部分用户。
健康产品中继占比通常就在 20–30%；**持续 >40% 要查区域部署或 UDP 被拦**。
中国家宽大量 CGNAT，容量按 **~30%** 估，不要按 5%。

对称 NAT × 对称 NAT（EDM×EDM）几乎打不穿 → CONNECT hint=`Relay`，1.5s 后开中继。
中继必须 **E2E 加密**（本仓库隧道 AEAD；对标 DERP 只转发密文、TURN 靠 DTLS-SRTP）。

### 1.4 首帧必须是 I 帧，预览 GOP 宜短

实时预览不是点播：播放器只能从 **IDR/I 帧** 起解。TUTK 文档：
`avRecvFrameData2` 丢帧/残帧后必须等到下一 I 帧；`avSendFrameData` 缓冲满
（`AV_ER_EXCEED_MAX_SIZE`）应丢后续 P、标记补 I。安防侧常见做法：
**GOP ≈ 帧率（1s 一个 I）**，不用 B 帧（zerolatency / baseline），否则重排序
把玻璃到玻璃延迟拉到数百毫秒。

本仓库：`AvFrameInfo.frame_type` 1=I / 0=P / 2=音频；拥塞时 `av_should_drop_p`
丢 P、保 I 和音频。

### 1.5 直播宁丢不卡：NACK 与 FEC 按 RTT 分工

打洞只解决「包能到」。1080p 预览还要在延迟预算里做抗丢包：

| 手段 | 适用 | 行业经验 |
|------|------|----------|
| NACK / 快重传 | 单向时延 ≲ 50ms（RTT ≲ 100ms） | 带宽省，但等一来回 |
| FEC | 高 RTT / 4G 突发丢包 | 不增加等待，有码率税 |
| too-late-drop | 直播预览 | 过期切片不再等（SRT 灵魂） |
| 自适应 jitter | 音频 15–120ms；视频可略大 | 快涨慢缩，避免振荡 |

一对一预览：**不可靠视频 + 关键 I 补发**；录像回放走 RDT/可靠通道。
音视频分线程收（TUTK 明确要求），避免音频阻塞视频重组。

接收侧不要无脑加大缓冲：TUTK「先缓存再解」适合回放；实时预览宜
**1–2 帧起播 + 按缓存水位帧控 sleep**。本仓库 `AvReassembler` 在新帧完成时
丢掉更旧的未完成帧。

### 1.6 网络会变，会话不能拆

手机 Wi-Fi ↔ 蜂窝、笔记本切网，NAT 绑定作废。WebRTC 用 **ICE restart**
（换 `ice-ufrag`/`ice-pwd`）：旧路径继续扛媒体，新路径 CONNECTED 再切，
间隙常 <1s；整段重建 PeerConnection 则要 1–3s（含 DTLS）。

Consent freshness：连通后仍周期性打 STUN 探活，断了走 `disconnected` →
超时 `failed` → 必须 restart。本仓库后台打洞 + `restart_ice` 对齐这条。

### 1.7 企业网只认 443

生产 TURN 必须兼 **3478/UDP 与 443/TCP+TLS**。只配 UDP STUN、不配中继，
是「connecting 永远转圈」的第一原因。区域化中继：单区 TURN 给远端用户
多 **50–200ms**。短时凭证（RFC 7635）防滥用；本仓库用 HMAC 注册 + 小时配额。

---

## 2. ICE / STUN / TURN ↔ 本仓库

```
WebRTC / RFC 8445                 本仓库
─────────────────                 ────────
Signaling（自选）                  NatServer CONNECT + MSG_ICE_SDP
STUN Binding → srflx              NatServer 应答 RFC 8489 Binding
                                  + 自研 MSG_NAT_DETECT（双 socket 判锥型）
ICE 候选收集 / 连通性检查          libjuice（host + srflx；检查中可出现 prflx）
TURN relay                        P2PProxy RELAY_DATA（配额 QuotaMB）
ICE 角色：offerer=controlling     CONNECT_OK 后发起方 gather；被邀方先 set_remote
iceTransportPolicy=relay          force_relay / peer -relay
LAN Search                        组播 239.255.77.89:17890（MSG_LAN_*）
选路 host > srflx > relay         PathSelect：ICE → TCP 打洞 → UDP 打洞
                                  → DERP TCP → UDP 中继
```

RFC 8445 类型优先级（host 126 > prflx 110 > srflx 100 > relay 0）：
**relay 故意垫底**。本仓库 `PathSelect` 同样：直连族优先，中继最后；
`force_relay` 跳过全部直连族。

### 2.1 角色（RFC 8445 §6.1.1）

WebRTC：**发 Offer 的一端 controlling**，负责提名。两端都 controlling
会触发 *role conflict*（libjuice 打日志 `ICE role conflict (both controlling)`），
靠 tie-breaker 恢复，本机回环上会偶发直连超时。

本仓库约定：

- `connect()`：**只建 agent，不 gather**。等 `CONNECT_OK`（对端已收到 INVITE）
  再 `start_ice_gather` → controlling。鉴权/Token 被拒时不会先把 SDP 发给对端。
- CONNECT_INVITE 被邀方：只建 agent，**先 `juice_set_remote_description` 再 gather**
  → libjuice 在 `AGENT_MODE_UNKNOWN` 时把本端定为 controlled
- `IceSdpMsg` 带 `src_uuid`：SDP 若早于 INVITE 到达，按源 UID 暂存，邀方创建后再应用

生产坑：**信令未成功就收集候选**。CONNECT 被 `NEED_AUTH` / `BAD_TOKEN` 拒绝时
对端没有会话，过期 SDP 会污染下一次连线。gather 绑在 CONNECT_OK 上就是为了这个。

红线（与 libjuice 死锁）：juice 状态/recv 回调里**不调任何 `juice_*`**；
持 `mu_` 的 tick 里不查 juice；`juice_set_remote_gathering_done` **仍在 tick 持锁**
（锁外会与回调并发，[3]/[5] 单边 CONNECTED）。restart 完成探测必须锁外
`juice_get_state`。详见计划书 §8。

### 2.2 STUN（RFC 8489）

libjuice 向 `stun_server_host:port` 发 Binding Request，用
**XOR-MAPPED-ADDRESS** 得到 srflx。本仓库把 **NatServer 兼做 STUN**：
收包线程识别 `0x0001 + magic 0x2112A442`，同 socket 回 Binding Success。
与自研 `XN` 头（`0x584E`）不冲突。

实现：`core/packet/StunBind.h`，`server/connectivity/StunResponder`。
客户端：公网时 `juice_config.stun_server_host = NatServer IP`。
**本机回环**（NatServer 为 `127.0.0.1` / `localhost`）不配 STUN：juice 默认
`MAX_STUN_SERVER_RETRANSMISSION_COUNT=5`，合计约 23.5s，会拖死 6s 直连窗口；
同机 host 候选已足够。回环时 `juice_config.bind_address=127.0.0.1`，避免 host
落在 eth0/docker 导致 controlling 只打到不可达地址、单向 CONNECTED。
若 juice 未回调 CONNECTED 但已收到应用数据，`on_juice_recv` 仍置 `direct_ok`
并 `set_connected`（路径已证明可达）。

NAT 探测另有 CHANGE-REQUEST / OTHER-ADDRESS（RFC 5780 风格）和 TCP STUN 同口，
二维结果进 `PunchAdmit`（锥×锥 Ice，EDM×EDM Relay hint）。

### 2.2.1 Trickle 与 gathering done

首包 SDP 通常只有 **host**。若此时立刻 `juice_set_remote_gathering_done`，
后续 srflx trickle 会被拒绝（日志 `Remote candidate added after remote gathering done`）。

- 首次 `set_remote` **不** mark done，记下 `ice_remote_applied_ms`
- 后续完整 SDP 按行 `juice_add_remote_candidate`，再 mark done
- 回环 400ms / 公网 25s 兜底（`tick_ice`），防止只收到一包时永远不收口
- 回环不发「首个 host」SDP：此时往往只有非 127.0.0.1 网卡，controlling 会打偏；
  等 gather 完成再发齐。连线中每 200ms 重传完整 SDP（最多 8 次），抗跨服丢包
- **跨服 ICE_SDP**：A 在 S1、B 在 S2 时，S1 经 `broadcast_sync_msg` 转发；
  `except=from` 防环。配了 `SyncAuthSecret` 时转发尾部带 HMAC，对端校验后剥掉再投递

### 2.2.2 `Conn` 必须堆分配

juice `user_ptr` 指向 `Conn`。`unordered_map<string, Conn>` 扩容会移动对象，
回调变成野指针。现为 `unordered_map<string, unique_ptr<Conn>>`，
`tick_connections` 先拷贝 key 再驱动。`juice_destroy` 必须在释放 `mu_` 之后：
回调要拿同一把锁，持锁销毁会与 juice 线程死锁（IOTC `stop()` 挂死）。
`IOTC_Connect` 超时锁外 `disconnect`，避免孤儿 ICE 让 `stop()` 挂死。

### 2.3 中继（TURN 角色，DERP 编排）

标准 TURN（RFC 8656）要 Allocate / ChannelBind。本仓库用更轻的
`MSG_PROXY_RELAY_DATA` 查表转发。NatServer **丢弃**该报文（`relay_reject`），
信令核不转发业务。

生产对齐：

- 中继按区域部署（`Region` / `ProxyRegions` / `pick_proxy` 同区加权）
- UDP 兼听 + TCP/TLS 443（对标 TURN-over-TLS / DERP）
- `QuotaMB` + 每小时窗口；`P2P_PROXY_AUTH_SECRET` 覆盖注册 HMAC

`p2p_proxy <port> <max> [workers] [QuotaMB] [AltPort]` 兼听第二端口
（生产填 443）。`ProxyAltPort` 作为额外 CONNECT 候选；客户端对所有候选
`PROXY_REGISTER`。`test.sh` [17] 只用 alt 端口走 `-relay`。

DERP TCP/TLS：`TcpPort` / `ProxyTcpPort`；未给 TcpPort 时 AltPort 兼听 TCP。
`test.sh` [18][19][20]。

### 2.4 中继回切 P2P 与 ICE restart

非 `force_relay` 时，中继建链后后台继续打洞，`direct_ok` 后 `send_tunnel_via`
改走 juice。`need_p2p` / 心跳 `np=1` 让对端满超时打洞（hint=`Need`），
**不能**盖掉 Relay hint，也**不能**打开 `force_relay`。

libjuice **不支持原地换 ufrag**。`P2PClient::restart_ice` /
`IOTC_Session_RestartICE` 重建 agent：旧 agent 挂在 `juice_prev` 继续扛媒体。
对端看到 `a=ice-ufrag` 变化即 `begin_ice_restart`（controlled）。新路径
CONNECTED 后回收旧 agent，再打一次 `on_connected`。worker **锁外**
`juice_get_state` 再回锁 `set_connected`。`peer -restart` / `test.sh` [16]。

---

## 3. 流媒体通道（打洞成功之后）

```
IOTC_Session
 ├─ ch0  IOCtrl（可靠）          ← DataChannel / RTCP 控制
 ├─ ch1  AV 视频（不可靠+FEC）    ← RTP + too-late-drop
 ├─ ch2  AV 音频
 ├─ ch3  RDT 文件（可靠+背压）
 └─ chN  P2PTunnel（RTSP/HTTP）
```

| 层 | 行业做法 | 本仓库 |
|----|----------|--------|
| 分片 | RTP / FU-A | `AvCodec` 11B 切片头（`client/sdk/iotc/AvCodec.h`） |
| 过期丢帧 | SRT too-late-drop | `AvReassembler` + `av_should_drop_p`（拥塞丢 P、保 I/音频） |
| 起播 | 必须从 I 帧；GOP≈1s | 应用填 `frame_type`；残帧等下一 I |
| 可靠控制 | DataChannel / RTCP | 通道 0 IOCtrl、RDT 字节流 |
| 拥塞 | GCC / TWCC / BBR | Session：RTO/快重传/cwnd；`TT_TWCC` + Kalman；`AbrController` AIMD |
| 加密 | DTLS-SRTP / QUIC | PSK 或 X25519 FS + AES-CTR（PSK 不进入 FS 密钥） |
| 中继时 | 降码、配额 | `QuotaMB` + ABR 目标码率 |

选路实现：`core/connectivity/transport/PathSelect.h`
（ICE nominated → juice_prev → juice+direct_ok → TCP 打洞 → UDP 打洞 → DERP TCP → UDP 中继）。

---

## 4. 局域网发现（已落地，对标 TUTK LAN Search）

同网段预览不应绕公网 STUN。行业做法是 **mDNS / SSDP / 私有组播** 先发现，
命中则 host 候选直连（<300ms）。

| 项 | 实现 |
|----|------|
| 组播组 | `239.255.77.89:17890`（TTL=1，环回开启，同机多进程可互发现） |
| QUERY | `MSG_LAN_QUERY`：按 UID 询问，空 UID=任意 |
| ANNOUNCE | `MSG_LAN_ANNOUNCE`：本机 UID + 主 socket 端口，周期 2s，缓存 15s |
| API | `P2PClient::lan_peers/lan_lookup`、`IOTC_Search_Device` |
| 与 ICE 配合 | 命中后写入 `have_lan`，SDP 额外经局域网单播一份 |

`-relay` / `force_relay` 不走 LAN 媒体，避免测试被直连抢走。
`P2P_DISABLE_LAN=1` 可关。

---

## 5. 现网观测（档 B 才有数）

没有这些埋点，§1 的数字无法验收：

| 指标 | 口径 | 用途 |
|------|------|------|
| 直连率 | **锥×锥与 EDM 分母分开** | 计划书 ≥85% 不含 EDM×EDM |
| 中继占比 | 会话数 / 字节数分开 | 健康 15–30%；>40% 查防火墙/区域 |
| TTFF / 出图 | LAN <300ms；打洞 <3s；含中继 <8s | 先通再优是否生效 |
| 选路 | `path_kind_str`（ice / derp_tcp / udp_relay…） | 确认没卡在次优 |
| 弱网 1080p | P50/P99、卡顿、丢 P 次数 | `tc netem` 或用户态切片丢失 |

本 CI 无 iptables/`ip`，`unshare` 被拒；`P2P_DISABLE_PORTMAP=1` 是默认
（无家宽 IGD）。现网矩阵不算内核未完成。

---

## 6. 仍建议补的能力

档 A 已落地：ICE restart、TURN-over-443、DERP TCP/TLS、Kalman+TWCC、
Token nonce、PortMap / PunchAdmit / PunchPolicy / TCP 打洞与 TCP STUN。

| 优先级 | 项 | 归属 |
|--------|----|------|
| B5 | 真 `tc netem` 1080p（卡顿 / P99）；无权限时继续用户态丢包 | 档 B |
| B3/B4 | 家宽 IGD 实机 + docker+iptables NAT 矩阵 | 档 B |
| B1 | 正式 443 证书、区域中继容量 | 档 B |
| C1–C3 | 多看客 SFU/28181、WHIP/WHEP、MoQ 笔记 | 档 C，**不进 NatServer** |

---

## 参考文献

- RFC 8445 ICE、RFC 8489 STUN、RFC 8656 TURN、RFC 8838 Trickle ICE、RFC 4787 NAT 行为
- WebRTC 生产：15–30% TURN、区域化中继、443/TLS、Trickle 缩短 TTFF、ICE restart 保媒体
- Tailscale *How NAT traversal works*：先 DERP 再升级直连（只学编排）
- TUTK：LAN/P2P/RLY 质量阶梯；`avSendFrameData` 满缓冲补 I；残帧等 I；实时预览浅缓冲+帧控
- SRT too-late-packet-drop；pion/ice 角色冲突
- [`P2P模型图谱.md`](P2P模型图谱.md)、[`流媒体优化学习路线.md`](流媒体优化学习路线.md)
