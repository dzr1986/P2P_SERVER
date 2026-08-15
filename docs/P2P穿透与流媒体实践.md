# P2P 流媒体穿透实践（对照本仓库）

> 对照 RFC 8445 ICE、RFC 8489 STUN、RFC 8656 TURN 与 WebRTC 生产经验，
> 说明本仓库「信令 + 打洞 + 中继 + 帧通道」如何对齐行业做法，以及还差什么。

---

## 1. 为什么 IPC 远程看流必须做 NAT 穿透

家用/运营商 NAT 默认**只允许内网主动发出的五元组回流**。摄像机在 NAT 后监听
UDP/TCP，公网 App 无法直接连入。行业有两条路：

| 路径 | 适用 | 代价 |
|------|------|------|
| 设备推流到云（RTMP/SRT → HLS/WebRTC 分发） | 一对多直播、回看 | 带宽始终走云，成本高 |
| **P2P 打洞 + 中继兜底**（TUTK / 本仓库） | 一对一预览、对讲 | 穿透成功后媒体不经云；失败才中继 |

一对一 IPC 预览应走第二条：NatServer 只做**信令与地址交换**（对标 WebRTC 的
signaling，协议自选），媒体尽量直连。一对多直播仍应走 CDN，不要用 mesh P2P。

生产经验（WebRTC 2025–2026）：**约 15–30% 会话必须中继**（对称 NAT、企业防火墙、
CGNAT）。没有 Relay 就等于放弃这部分用户。本仓库 `P2PProxy` 即 TURN 角色。

---

## 2. ICE / STUN / TURN 三件套 ↔ 本仓库

```
WebRTC / RFC 8445                 本仓库
─────────────────                 ────────
Signaling（自选）                  NatServer CONNECT + MSG_ICE_SDP
STUN Binding → srflx              NatServer 应答 RFC 8489 Binding
                                  + 自研 MSG_NAT_DETECT（双 socket 判锥型）
ICE 候选收集 / 连通性检查          libjuice（host + srflx）
TURN relay                        P2PProxy RELAY_DATA（配额 QuotaMB）
ICE 角色：offerer=controlling     CONNECT_OK 后发起方 gather；被邀方先 set_remote
LAN Search                        组播 239.255.77.89:17890（MSG_LAN_*）
```

### 2.1 角色（RFC 8445 §6.1.1）

WebRTC：**发 Offer 的一端 controlling**，负责提名候选对。两端都 controlling
会触发 *role conflict*（libjuice 打日志 `ICE role conflict (both controlling)`），
靠 tie-breaker 恢复，但在本机回环上会偶发直连超时。

本仓库约定（与 WebRTC Offer/Answer 对齐）：

- `connect()`：**只建 agent，不 gather**。等 `CONNECT_OK`（对端已收到 INVITE）
  再 `start_ice_gather` → controlling。鉴权/Token 被拒时不会先把 SDP 发给对端。
- CONNECT_INVITE 被邀方：只建 agent，**先 `juice_set_remote_description` 再 gather**
  → libjuice 在 `AGENT_MODE_UNKNOWN` 时把本端定为 controlled
- `IceSdpMsg` 带 `src_uuid`：SDP 若早于 INVITE 到达，按源 UID 暂存，邀方创建后再应用

生产上还有一条易踩的坑：**信令未成功就收集候选**。CONNECT 被 `NEED_AUTH` /
`BAD_TOKEN` 拒绝时对端没有会话，过期 SDP 会污染下一次连线（同一设备连续被
多个客户端试连时尤其明显）。本仓库把 gather 绑在 CONNECT_OK 上就是为了避免这个。

### 2.2 STUN（RFC 8489）

libjuice 向 `stun_server_host:port` 发 Binding Request，用
**XOR-MAPPED-ADDRESS** 得到公网映射（srflx）。本仓库把 **NatServer 兼做 STUN**：
收包线程识别 `0x0001 + magic 0x2112A442`，同 socket 回 Binding Success。
与自研 `XN` 头（`0x584E`）不冲突。

实现：`common/StunBind.h`，`NatServer::recv_thread` 快速路径。
客户端：公网时 `juice_config.stun_server_host = NatServer IP`。
**本机回环**（NatServer 为 `127.0.0.1` / `localhost`）不配 STUN：juice 默认
`MAX_STUN_SERVER_RETRANSMISSION_COUNT=5`，合计约 23.5s，会拖死 6s 直连窗口；
同机 host 候选已足够，gather 应立刻完成。

### 2.2.1 Trickle 与 gathering done

首包 SDP 通常只有 **host** 候选。若此时立刻
`juice_set_remote_gathering_done`，后续 srflx trickle 会被 juice 拒绝
（日志 `Remote candidate added after remote gathering done`）。本仓库：

- 首次 `set_remote` **不** mark done，记下 `ice_remote_applied_ms`
- 后续完整 SDP 按行 `juice_add_remote_candidate`，再 mark done
- 回环 400ms / 公网 25s 兜底（`tick_ice`），防止只收到一包时永远不收口

### 2.2.2 `Conn` 必须堆分配

juice `user_ptr` 指向 `Conn`。`unordered_map<string, Conn>` 扩容会移动对象，
回调变成野指针（随机崩溃、心跳 `pub_ip_` 乱码）。现为
`unordered_map<string, unique_ptr<Conn>>`，`tick_connections` 先拷贝 key
再驱动，避免 `close_conn` 边遍历边删。`juice_destroy` 必须在释放 `mu_`
之后做：回调要拿同一把锁，持锁销毁会与 juice 线程死锁（IOTC `stop()` 挂死）。

### 2.3 TURN / 中继

标准 TURN（RFC 8656）要 Allocate / ChannelBind。本仓库用更轻的
`MSG_PROXY_RELAY_DATA` 查表转发，语义相同：**直连失败保证可达**。
生产建议：

- 中继按区域部署（已有 `Region` / `ProxyRegions` / `pick_proxy` 同区加权）
- 企业网常拦非 443 UDP：后续可把 Proxy 挂到 UDP/443 或 TCP/TLS（对标 TURN-over-TLS）
- 计量限速已有 `QuotaMB`，避免中继被打满

### 2.4 中继回切 P2P

WebRTC 的 ICE restart / 持续探测：网络从蜂窝切 Wi-Fi 后应重新打洞。
本仓库：非 `force_relay` 时，中继建链后后台继续打洞，`direct_ok` 后
`send_tunnel_via` 改走 juice，不再占中继带宽。

---

## 3. 流媒体通道（打洞成功之后）

穿透只解决「包能到」。1080p 预览还要：

| 层 | 行业做法 | 本仓库 |
|----|----------|--------|
| 分片 | RTP / FU-A | `AvCodec` 11B 切片头 |
| 过期丢帧 | SRT too-late-drop | `AvReassembler` + `av_should_drop_p`（拥塞丢 P、保 I/音频） |
| 可靠控制 | DataChannel / RTCP | 通道 0 IOCtrl、RDT 字节流 |
| 拥塞 | GCC / TWCC / BBR | Session：自适应 RTO、快重传、cwnd；`avSuggestedBitrateKbps` |
| 加密 | DTLS-SRTP / QUIC | PSK 或 X25519 FS + AES-CTR |

一对一预览优先 **不可靠视频 + 可靠 I 帧关键补**；录像回放走 RDT/可靠通道。

---

## 4. 局域网发现（已落地，对标 TUTK LAN Search）

同网段 IPC 预览不应绕公网 STUN。行业做法是 **mDNS / SSDP / 私有组播** 先发现，
命中则 host 候选直连（&lt;300ms），未命中再走 ICE+STUN。

本仓库：

| 项 | 实现 |
|----|------|
| 组播组 | `239.255.77.89:17890`（TTL=1，环回开启，同机多进程可互发现） |
| QUERY | `MSG_LAN_QUERY`：按 UID 询问，空 UID=任意 |
| ANNOUNCE | `MSG_LAN_ANNOUNCE`：本机 UID + 主 socket 端口，周期 2s，缓存 15s |
| API | `P2PClient::lan_peers/lan_lookup`、`IOTC_Search_Device` |
| 与 ICE 配合 | 命中后写入 `have_lan`，SDP 额外经局域网单播一份（NatServer 慢也不堵） |

模式仍是 **LAN > P2P > Relay**。`-relay` / `force_relay` 不走 LAN 媒体，避免测试被直连“抢走”。

---

## 5. 仍建议补的能力（按收益）

1. **ICE restart**：`juice` 当前不支持换 ufrag；网络切换可重建 agent
2. **TURN-over-443**：穿透企业防火墙
3. **完整 GCC / TWCC**：`AbrEstimate.h` 已是 delay-based 分档；下一步用 RTT 趋势做 AIMD
4. **真实 `tc netem` 1080p**：卡顿率 / P99 延迟验收（P3）
5. **Token nonce 防重放表**：连线 Token 已验签，尚未记已用 nonce

参考文献：RFC 8445 / 8489 / 8656；pion/ice 角色冲突处理；
WebRTC 生产经验（15–30% TURN、区域化中继、443/TLS）；
TUTK LAN Search / SSDP 同网段发现。
