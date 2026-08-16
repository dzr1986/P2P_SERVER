# P2P 流媒体服务器：还缺什么

> 更新：2026-08-16。检索：WebRTC 生产 12 件套（2026）、TUTK Master/KDC/分区、
> go2rtc+coturn 自建摄像头栈、W3C webrtc-stats。
>
> 问题：按**当前 `p2p_server` 架构**，要做成可售的「P2P 流媒体服务器」，还缺哪一层？
> 结论先写：**连接内核已收口；缺的是平台面、会话遥测和 1:N 旁路，不是再拆 NatServer。**

配套：[`P2P服务器开发计划书.md`](P2P服务器开发计划书.md)、
[`P2P穿透与流媒体实践.md`](P2P穿透与流媒体实践.md)、
[`P2P模型图谱.md`](P2P模型图谱.md)。

---

## 1. 先分清三个「服务器」

行业把「流媒体服务器」说成一件东西。本仓库必须拆开，否则会把 SFU、计费、录像
全塞进 `p2p_natserver`。

| 名字 | 行业对应 | 本仓库 | 状态 |
|------|----------|--------|------|
| **连接服务器** | TUTK P2P Server；WebRTC signaling + STUN | `p2p_natserver` | 档 A 已收口 |
| **中继服务器** | TURN / Tailscale DERP | `p2p_proxy` | 档 A 已收口 |
| **流媒体服务器** | SFU / go2rtc / 国标网关 | **尚未立项**（档 C） | 1:N 才需要 |
| **平台面** | TUTK Master + KDC；License / 分区 / 计量 | **尚未立项**（档 D） | 要「可售」才缺 |

当前三进程只覆盖前两行：

```
设备/App ──信令──► p2p_natserver（建连，不转发业务）
                ├─ 直连媒体（不经服务器）
                └─ 失败 ──► p2p_proxy（密文转发）
离线 ──► p2p_wakeserver
```

一对一预览：**媒体不应经过「流媒体服务器」**。这是 P2P 的产品特点，不是缺口。
缺口出现在：多看客、浏览器、国标、计费控制台、现网可观测。

---

## 2. 行业清单对照（12 件套 × TUTK）

生产 WebRTC 系统周围有 12 个组件（signaling、media gateway、TURN、recording、
transcoding、CDN、observability、billing、E2E、compliance、guardrails、deployment）。
TUTK 另有 Master / License Key / ServerKey / VPG / 区域锁定 / KDC 带宽台账。

| # | 组件 | 本仓库 | 缺口归属 |
|---|------|--------|----------|
| 1 | 信令 | NatServer CONNECT + ICE_SDP + RegistrySync | 够用 |
| 2 | STUN | NatServer Binding + StunResponder | 够用 |
| 3 | TURN/中继 | Proxy UDP + TCP/TLS 443 + HMAC + QuotaMB | 现网证书/容量 = 档 B |
| 4 | 1:1 媒体拓扑 | 设备直连 App；禁止 mesh | **正确**；不要改成一律进 SFU |
| 5 | 1:N 媒体网关 | 无 | 档 C：独立 SFU，设备仍只出一份 |
| 6 | E2E 加密 | AEAD + X25519 FS | 够用 |
| 7 | 防滥用 | AntiAbuse、JailFile、PrivateMode、ConnectToken | 够用；WakeTrigger HMAC 见小项 |
| 8 | 节点观测 | `GET /` + `/metrics`（在线、CONNECT、punch hint、proxy_up） | **会话级**路径/TTFF/NAT 分母未出 = 档 D3 |
| 9 | 计费 / 控制台 | Proxy 有 QuotaMB，无按 UID 字节台账、无 KDC | 档 D2 |
| 10 | Master / 许可证 | `uidgen` + License 白名单 | 无「授权这台 P2P 服务器、解析入口」= 档 D1 |
| 11 | 区域合规 | 节点 `Region` + 就近 Proxy | 无客户端锁区（GDPR/cn）= 档 D4 |
| 12 | 部署 | 二进制 + cfg | 无 compose / 正式证书 / 手册 = 档 B1 |
| 13 | 录像 / 转码 / CDN | 明确非目标 | 不要做进连接核 |
| 14 | 推送 / App 账号 | 明确非目标 | FCM/APNs 在应用层 |
| 15 | 唤醒 | wakeserver | 出图 <6s = 档 B6 |

go2rtc/Frigate 路线是 **LAN RTSP → 本机 WebRTC 网关 → 可选 coturn**，没有 UID。
本仓库对标 Kalay：**公网 UID 连线**。两种栈在 1:N/浏览器处汇合（档 C 用 go2rtc，不要重写 IOTC）。

---

## 3. 按层看缺口

### 3.1 连接核（`server/`）— 不是主缺口

已有：报到/CONNECT/STUN、PunchAdmit、RegistrySync、RelayHealth、StatusAllow、
NatServer 丢弃 `MSG_PROXY_RELAY_DATA`。

仓库内小项（不挡「流媒体服务器」立项）：

1. Proxy TCP 会话可 join 的停机
2. WakeTrigger 可选 HMAC
3. 心跳压测器接到独立 CI

**不要再往 `instance/NatServer.cpp` 堆 Master、SFU、计费 SQL。**

### 3.2 中继 — 缺运营，不缺协议

已有 DERP 面和配额窗口。生产还要：

- 正式 443 证书、多区域 Anycast 或就近（B1）
- **按 UID 的中继字节**导出（D2）；QuotaMB 是限速不是台账
- 中继占比仪表：健康 15–30%，持续 >40% 查 UDP 被拦（实践文档 §1.3）

### 3.3 媒体约定 — SDK 有通道，产品约定未写死

通道 ch0–chN、too-late-drop、TWCC/ABR 已有。IPC 产品还缺**约定**（不必改信令核）：

| 约定 | 行业 | 建议 |
|------|------|------|
| 主码 / 子码 | 预览走子码，录像走主码；TUTK 多 avIndex | 文档约定 ch1=子码预览、另通道主码；或 IOCtrl 切流 |
| 关键帧请求 | RTCP PLI/FIR；TUTK 丢帧后等 I、满缓冲补 I | IOCtrl「请发 I」（D6）；服务端不解码，只当需要可计数 |
| 起播 | 必须 I 帧；GOP≈1s；不用 B 帧 | 写进设备集成指南，不是服务器功能 |
| 对讲 | 独立音频通道 + 回声 | 应用层；网关双声道是档 C |
| 1:N | mesh 会把设备上行打满 | **禁止**；第三路起走 SFU（C1） |

simulcast/SVC 是 SFU 侧的事。1:1 P2P 用双码流即可，不要在设备上编码三层给一个手机。

### 3.4 平台面（档 D）— 主缺口

TUTK 把「Master」和「P2P 服务器」分开：Master 验 UID、管一批 P2P 服务器、分区；
P2P 服务器管报到和协助连线。本仓库把 Master 职责压缩成离线 `uidgen` + 配置白名单，
**私有化单集群够用，多租户/多区域售卖不够用。**

| ID | 缺什么 | 为什么要 | 不做什么 |
|----|--------|----------|----------|
| **D1 Master** | 服务器登记（ServerKey）、入口解析、UID 是否允许在这台节点报到 | 对标 License Key / 私有化多节点授权 | 不做成 DHT；不进 NatServer 热路径 |
| **D2 计量** | 中继字节 × UID × 小时；只读控制台 | KDC；TURN 是第一成本 | 不对直连流量计费（也计不到） |
| **D3 会话遥测** | 每条 CONNECT：`path_kind`、TTFF、mapping/filter、是否中继升直连 | 没有分母就无法报「锥×锥 85%」 | 不要把码流塞进日志 |
| **D4 锁区** | 客户端/设备 `region=cn\|eu\|…`，与节点 Region 不一致则拒绝 | GDPR / 跨境 | 现有 1 字符 Region 调度保留 |
| **D5 分组** | VPG / 租户：这台服务器只服务一批 UID | 多客户私有化 | 现有 License 白名单可当 v1 |
| **D6 PLI** | IOCtrl 请求 I 帧 | 花屏恢复；对标 `pliCount` | 不要在 Proxy 里解析 H.264 |

D3 应最先做：现网档 B 的直连率、中继占比都依赖它。客户端已有 `PathSelect`，
缺的是 **CONNECT 结束时上报一条，NatServer/Proxy 各记计数器**（按 path / NAT，
**不要**把 instance 塞进 `p2p_node_region`）。

### 3.5 现网（档 B）— 缺环境

压测机、证书、家宽 IGD、`tc netem`、三平台 SDK、唤醒真机。
清单见计划书 §7.1。**不算连接核未完成。**

### 3.6 旁路（档 C）— 缺独立进程

3 路同看、浏览器、28181：用 go2rtc / mediasoup / ZLMediaKit 之一，
WHIP/WHEP 进网关。设备上行仍一份。**禁止进 `p2p_natserver`。**

---

## 4. 建议开发顺序

```
档 A 连接核 ──已收口──┐
                      ├─► D3 会话遥测（小改，让 B 有分母）
                      ├─► B1 部署 + 443 证书
                      ├─► D2 中继按 UID 导出（Proxy 已有配额，补时序库）
                      ├─► D1/D4/D5 Master 与锁区（要卖多区域再做）
                      ├─► D6 关键帧 IOCtrl（设备集成时做）
                      └─► 档 C 仅当出现 1:N / 浏览器硬需求
```

| 若目标是… | 先做 |
|-----------|------|
| 私有化 1:1 预览上线 | D3 + B1 + B2，不必 SFU |
| 对标 Kalay 可售平台 | 上项 + D1 + D2 + D4 |
| App 里三路同看 / Chrome 免 SDK | 档 C，设备不要 mesh |
| 云录像 / 推送 / 账号 | 继续列为非目标 |

---

## 5. 观测：节点指标 vs 会话指标

现在 `/metrics` 能回答「这台 NatServer 活着吗、有多少在线、CONNECT 成败、
punch 策略计数、Proxy 是否 up」。

还不能回答（W3C getStats / 现网运营常用）：

| 问 | 缺的序列 |
|----|----------|
| 直连还是中继？升直连了吗？ | `path_kind`（ice / derp_tcp / udp_relay…） |
| 锥×锥成功率？ | CONNECT 双方 `mapping`×`filter` 分母 |
| 用户等多久看到第一帧？ | TTFF（信令 OK → 首个 I 帧） |
| 中继是不是在吃钱？ | Proxy 字节 / UID |
| 花屏是否在等 I？ | 应用 PLI 计数（D6） |

NatServer 看不到帧。TTFF / PLI 只能由 **客户端或网关** 打点，服务端做汇总。
不要让信令核解析视频。

---

## 6. 明确不要做进 p2p_server

| 项 | 原因 |
|----|------|
| SFU / MCU / HLS / 转码 | 拓扑不同；MCU 太贵 |
| 设备侧 mesh 多看客 | 上行 O(N) |
| TUN / VPN / 魔法 DNS | 不是 IoT UID |
| 云录像、推送、账号 | Kalay 另一条产品线 |
| 在 juice 回调里发媒体统计 | 死锁红线 |
| 改 `force_relay` | 测试语义 |

---

## 7. 一页对照：有 / 缺

| 能力 | 有 | 缺 |
|------|----|----|
| UID 报到 + CONNECT + STUN | ✓ | Master 授权集群 |
| ICE + DERP 先通再切 | ✓ | 现网 443 证书 |
| 1:1 AV/RDT/Tunnel | ✓ | 主/子码与 PLI 约定 |
| 节点 /metrics | ✓ | 会话 path / TTFF / NAT 分母 |
| Proxy 配额 | ✓ | 按 UID 计费导出 |
| Region 就近 | ✓ | 客户端锁区 |
| License 白名单 | ✓ | 多租户 VPG |
| 唤醒协议 | ✓ | <6s 真机 |
| 1:N / 浏览器 / 国标 | — | 独立网关 |

**一句话**：`p2p_server` 已经是合格的 **P2P 连接服务器**；要成为 **P2P 流媒体平台**，
补档 D（遥测与计量优先）和档 B（证书与现网）；要成为 **多看客流媒体服务器**，另开档 C，
不要把媒体面焊回信令核。
