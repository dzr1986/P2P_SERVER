# P2P 连接模型图谱（安防 / IoT / 实时媒体）

> 本文把互联网上能查到的、和「设备远程看流」相关的 P2P 模型摊开，
> 对照本仓库该学什么、不该抄什么。配套：
> [`P2P服务器开发计划书.md`](P2P服务器开发计划书.md)、
> [`P2P穿透与流媒体实践.md`](P2P穿透与流媒体实践.md)、
> [`流媒体优化学习路线.md`](流媒体优化学习路线.md)。
>
> 检索基线：2026-04～2026-08（NVR P2P 综述、TUTK 官网、webrtcHacks MoQ、
> Cloudflare MoQ、Iroh FAQ、Tailscale DERP、libp2p DCUtR 论文、
> EasyTier 打洞、组网 VPN 对照、PPPP/iLnk 族、
> GB/T 28181↔WebRTC、WHIP RFC 9725 / WHEP RFC 9737）。

---

## 1. 安防远程看流的四种商业模式

综述（mickeyzzc / NVR P2P 对比）把市面方案分成四档。本仓库对标的是 **C 的可私有化加强版**。

| 模式 | 代表 | 谁转流 | 谁拥有数据 | 本仓库态度 |
|------|------|--------|------------|-----------|
| A 封闭云中继 | 海康/大华官方云、萤石、Reolink、Agent DVR | 厂商云 | 厂商 | 不抄；带宽成本转嫁给云 |
| B 可自建 Hub | Kerberos.io（MQTT 低清快照 + WebRTC 高清） | 自建或托管 | 用户 | 一对多观看可借鉴「双通道」 |
| C 纯 P2P + 自建 TURN | go2rtc / Frigate + coturn | 直连，失败走自建中继 | 用户 | **本仓库主路径** |
| D 无远程 | 端口映射 / VPN / 只 LAN | 无 | 用户 | 仅作回退，不是产品 |

TUTK Kalay 是 **C 的商业化形态**：UID + 信令 + 打洞 + 中继兜底，服务器可私有化。
海康/大华自研 P2P 也是 C，但协议封闭、设备端极轻（嵌入式资源紧）。

---

## 2. 穿透率：公开数字对照本仓库目标

本仓库计划书目标：**锥型组合 P2P ≥ 85%，含中继整体 ≥ 99.9%**。
公开数字用来校准预期，不是 KPI 承诺。

| 方案 | 公开/综述数字 | 备注 |
|------|---------------|------|
| TUTK Kalay | ~92% 直连；对称 NAT 号称 85%+ | 商业对标；方法未公开 |
| 家宽摄像头（Reolink/海康综述） | ~95% 直连 | 双方都是家宽锥型时很高 |
| 移动 4G 摄像头 | 失败率可到 98% | 运营商对称 NAT / CGNAT |
| 标准 WebRTC / go2rtc | ~80%；对称 NAT ~20% 失败 | 无厂商私有打洞技巧 |
| libp2p DCUtR（IMC 2026 / ProbeLab） | **70% ± 7.1%**（440 万次 / 8.5 万网 / 167 国） | 去中心化、无全局协调；端口受限锥 82.9%，对称 ~40% |
| Iroh / Tailscale | 号称 >90% | 先经中继通，再升级直连 |
| 萤石开放 SDK | ~20% P2P | 多数走云中继 |

**中国宽带现实**：综述称 >95% 家宽在 CGNAT 后。这意味着：

- 「打洞成功」不能当默认；**中继是一等公民**，不是「偶尔兜底」。
- 容量规划按 **15–30% 会话走中继** 做（国内可按 30%），不要按 5% 估。
- 对称 NAT × 对称 NAT **几乎打不穿**（见 §4 生日悖论），必须中继。

---

## 3. NAT 模型：别再用「四型」做唯一分类

### 3.1 RFC 4787 二维模型（应作为内部术语）

旧四型（Full Cone / Restricted / Port Restricted / Symmetric）把
**映射**和**过滤**绑在一起，和真实盒行为对不齐。RFC 4787 拆成两维：

| 维 | 取值 | 对打洞的含义 |
|----|------|--------------|
| 映射 | EIM 终点无关 / ADM 地址相关 / EDM 地址+端口相关 | 只有 EIM 才能稳定打洞 |
| 过滤 | 无 / 地址 / 地址+端口 | 决定对方要从哪个五元组回包 |

**本仓库动作**：日志与监控用 `mapping=EIM|ADM|EDM` + `filter=none|addr|port`
（`P2PClient::nat_mapping/nat_filter/nat_port_step`）。四型只作对外通俗说法。
第三探测口识别 NAT4E 步长；`NatMatrix.h` 给出两端策略。ADM 需第二公网 IP，暂记 EIM。

### 3.2 生日悖论打洞（对称 / EDM）

一边开 N 个源端口，另一边探 N 个目的端口，碰撞概率约 `1 - e^{-N²/R}`
（R 为 NAT 端口池，常按 65535）。公开估算：

| N（每侧） | 约碰撞率 | 备注 |
|-----------|----------|------|
| 256 | ~64% | EasyTier / APNIC 常用起点 |
| 2048 | ~99.9% | 论文上限；NAT 表压力与 IDS 风险陡增 |

| 组合 | 策略 | 期望 |
|------|------|------|
| EIM × EIM | 标准 ICE（本仓库 juice） | 高 |
| EIM × EDM | 生日：EIM 侧多探，EDM 侧多开源口 | 中 |
| EDM × EDM | 几乎无解；双端生日也贵 | 必须中继（默认） |

**代价**：扫描会触发 IDS/运营商风控。EasyTier 提供 `--disable-sym-hole-punching`，
对称×对称失败后走中继，但仍允许对称×锥型用普通逻辑。P8 必须默认关、有上限、间隔、熔断。

### 3.3 增量对称 NAT（NAT4E）端口预测

部分对称 NAT **按连接递增/递减分配端口**（EasyTier 称 NAT4E）。
不必扫 256 口，按步长预测下一映射即可。P8 二维探测若识别到单调端口，优先预测、再生日。

### 3.4 主动开孔：PCP / NAT-PMP / UPnP IGD

家宽路由若支持，设备可 **向自家 NAT 要一个固定外网口**，把 EDM 变成近似 EIM。
这是「免费」的穿透率提升，不增加服务器成本。P8 **优先于**生日扫描。

EasyTier 顺序可直接抄：**先 IGD(UPnP) → 失败再 NAT-PMP/PCP → 租约续期（约 300s，240s 续）**。
注意：不少路由一个「关 UPnP」开关会把 PCP/NAT-PMP 一起关掉；失败是常态，不能当唯一路径。

### 3.5 IPv6 / NAT66

有公网 IPv6 时 host 候选即可直连，应优先于 IPv4 打洞。
运营商开了 NAT66 则与 IPv4 NAT 同类，不能假设「有 v6 就能通」。
ICE 已收集 v6 host；P8 监控应分开统计 v4/v6 直连率。

### 3.6 TCP 打洞与「UDP 更好」迷思

DCUtR（IMC 2026）与后续综述：**同步好的 TCP 打洞率与 UDP/QUIC 相当**。
企业网常只放行 TCP/443。本仓库已有 UDP ICE + UDP/443；
P8 要补的是 **TCP 打洞或至少 TCP/TLS 中继**，不是再优化一轮纯 UDP。

---

## 4. 连接编排模型（信令 + 中继怎么配合）

这是本仓库最该学的一层：**不是「先打洞、失败再中继」**，而是「先保证通，再升级直连」。

### 4.1 经典 ICE（本仓库现状）

```
CONNECT 信令 → 并行采集 host/srflx/relay → 连通性检查 → 提名 → 媒体
打洞超时（~8s 目标）→ 只用 relay
```

优点：标准、和 WebRTC 互通潜力大。
缺点：企业防火墙丢 UDP 时，**在 ICE 完成前用户看到黑屏**。

### 4.2 Tailscale DERP 模型（强烈建议学编排，不抄 VPN）

```
TCP/443 连最近 DERP → 立刻有一条可用通路（延迟高、能用）
并行 UDP 打洞 → 成功则无缝切直连，DERP 只作保底
```

要点：

- 中继走 **TLS/443**，企业防火墙很难拦（比只兼听 UDP/443 更抗墙）。
- 「先通再优」比「先优再兜底」体验好一个数量级。
- Tailscale 是 **系统级 WireGuard VPN**；本仓库是 **应用内一条会话**。只学编排。

本仓库已有：UDP Proxy + `AltPort`（443）+ **TCP/TLS DERP 面**（`TcpPort` / `ProxyTcpPort`）。
客户端在 CONNECT 后（或 `-tcp`）立刻建 TLS 会话并注册，媒体可马上走中继；
ICE 并行打洞，`direct_ok` 后 `send_tunnel_via` 切 `juice_send`，再回调 `on_connected(relay=false)`。

### 4.3 Iroh 模型（应用内嵌 QUIC，不是系统 VPN）

- 每个节点一个 **node-id**（公钥），地址当提示不是身份。
- 内嵌 QUIC，NAT 打洞 + relay 升级直连。
- 和 Tailscale **分层不同**：Iroh 不做整机组网。
- 本仓库 UID ≈ Iroh node-id 的「可读/可签发」版本；底层仍是 ICE+自研帧，不必换 QUIC 重写。

### 4.4 libp2p DCUtR（去中心化打洞）

- 先经 **circuit relay** 通信，再协调打洞升级。
- IMC 2026：全球直连率仅 ~70%；**TCP 与 QUIC 打洞率几乎一样**
  （打破「UDP 一定更好」的迷信）。
- 本仓库是 **有中心信令的 IoT**，不要做成 DHT。论文价值是：
  **有全局协调时，应显著高于 70%**；若长期低于此，是实现问题不是模型问题。

### 4.5 EasyTier（开源 SD-WAN，打洞策略最值得抄）

不是 IoT SDK，但是 **开源里把打洞策略写得最全** 的一套：

| 策略 | 行为 | 本仓库 |
|------|------|--------|
| 开孔优先 | UPnP → NAT-PMP，成功则宣告映射口 | P8 第一项 |
| 锥×锥 | 对已发现公网口互发 | 已有 ICE |
| 对称×锥 | 对称侧打锥侧开口 | ICE 部分覆盖 |
| 对称×对称 | 生日 / 可关 | P8 可选，默认关 |
| NAT4E | 端口预测 | P8 探测字段已落地；ICE 预测路径待接 |
| TCP 打洞 | 与 UDP 并行，可单独关 | P8 |
| `--lazy-p2p` | 无业务流量不后台打洞 | 省电 IPC 可学（与 P7 唤醒配合） |
| 失败中继 | 自动回退 | 已有 Proxy |

只学策略与开关设计，不把本仓库做成 SD-WAN。

### 4.6 组网 VPN 族（分层不同，只学编排）

2026 对照（Tailscale / Headscale / ZeroTier / Nebula / NetBird / OpenZiti）：

| 产品 | 控制面 | 数据面 | 打洞失败 |
|------|--------|--------|----------|
| Tailscale / Headscale | 中心协调 | WireGuard | DERP（HTTPS/443）先通再切 |
| NetBird | 自建 signal + relay | WireGuard | TURN |
| ZeroTier | Planet Root + Controller | 自研 VL1 | Root / Moon 转发 |
| Nebula | 自建 Lighthouse | Noise | **仅 UDP**；中继要自己指定 |
| OpenZiti | Controller + Router | 自研 | Edge Router 转发 |

共同点：**控制面只交换地址/密钥，媒体 E2E；节点不为别人转发（专用中继除外）**。
Nebula 的弱点正好是本仓库 P8 要补的：没有 TCP/443 面时，酒店/企业网会黑屏。

**本仓库不是 VPN**：不做虚拟网卡、不做整机组网。UID 会话 = 应用内一条连接。

### 4.7 厂商私有编排（只学策略）

| 厂商 | 策略 | 可吸收 |
|------|------|--------|
| 海康 | 自研轻量 P2P，嵌软省资源 | 设备端少线程、少 fd |
| 大华 | 调解服务器看双方 NAT：都 LAN / 单侧 NAT / 双侧 NAT | 决策树：LAN 优先 → 单侧打洞 → 双侧中继 |
| 群晖 QuickConnect | 本机回环 → 直连 → 中继 三步回退 | 和本仓库 LAN/P2P/Relay 一致 |
| UniFi Protect | 标准 WebRTC + Twilio TURN | 证明「标准 ICE + 商用 TURN」能卖 |
| TUTK vs WebRTC（官网对照） | 自称对称 NAT 也能打、嵌软/RTOS 轻、少依赖中继 | 学「轻 SDK + 高穿透」目标；数字未公开方法，当宣传 |

### 4.8 廉价摄像头 PPPP 族（反面教材，禁止抄安全）

同一设计血统的封闭协议（综述 / 逆向）：

| 变体 | 典型库 | 备注 |
|------|--------|------|
| CS2 Network | `libPPCS_API` / `PPPP_ConnectByServer` | 低端机最常见 |
| 小米/Yi | `PPPP_API` | 云 + P2P 混用 |
| iLnk / 云视通 | `XQP2P_*` / `libHiChipP2P` | 硬编码密钥、可伪造 UID 的公开事故 |
| HLP2P | `HLP2P_ConnectByServer` | 同源变体 |
| TUTK Kalay / PPCS | IOTC SDK；go2rtc 有 `pkg/tutk` | 商业正统；历史也有 CVE |

**本仓库对标的是 Kalay 的产品形态（UID + 通道 + 可私有化），不是 PPPP 明文/硬编码。**
安全红线见 §7。一对多观看可学 go2rtc：一条 UID 会话进网关，再扇出 RTSP/WebRTC（P9）。

---

## 5. 媒体拓扑模型（一对一 vs 一对多）

**P2P 只适合 1:1 或极小会议室。** 安防里「一台 IPC、五个手机同时看」不该 mesh。

| 拓扑 | 适用 | 服务器负载 | 本仓库 |
|------|------|------------|--------|
| Mesh P2P | 1:1 预览、对讲 | 近 0 | **主路径** |
| 设备当 SFU（多路编码） | 2–3 路 | 设备 CPU 爆 | 嵌入式禁止 |
| 云/边缘 SFU | 4+ 路同看、Web 墙 | 中 | P9：旁路网关，不改 1:1 协议 |
| MCU 混流 | 会议合成 | 高 | 不做 |
| CDN / HLS / DASH | 回看、公开直播 | 高 | 非目标 |
| MoQ（Media over QUIC） | 大规模直播分发 | 中（中继可 CDN 化） | P11 预研，**不替代 1:1** |

webrtcHacks（2026-03）：>4 人不要 WebRTC mesh；万人活动用 MoQ 或 HLS。
Cloudflare 已把 MoQ 当「实时 + CDN 中继」方向。本仓库 IPC 预览仍是 1:1 P2P。

### 5.1 双通道（Kerberos / 部分 NVR）

- **控制 / 预览**：可靠或 MQTT，小图/事件，不怕丢。
- **高清实时**：不可靠 + FEC/NACK，怕延迟。

本仓库已有 AV（不可靠）+ RDT/IOCtrl（可靠）+ Tunnel。P9 可明确：
多看客时「第一路 P2P 高清，其余走网关转推」，避免设备发 N 份码流。

---

## 6. 接入与互通模型

| 模型 | 标准 | 用途 | 与本仓库关系 |
|------|------|------|--------------|
| IOTC UID 会话 | 对标 TUTK | IPC↔App 1:1 | **主协议，已落地** |
| WHIP | RFC 9725 | HTTP POST SDP 推流进 WebRTC | P10：浏览器/OBS 推入网关 |
| WHEP | RFC 9737 | HTTP POST SDP 拉流 | P10：浏览器不装 SDK 看流 |
| GB/T 28181 | 国标 SIP + PS 流 | 公安/政务平台 | P9：网关互转，设备端不重写国标栈 |
| WebRTC 原生 | ICE/DTLS/SRTP | 浏览器 | 网关侧，不替换 juice+自研帧 |
| RTSP / ONVIF | 设备本地 | 局域网 NVR | Tunnel 已能打通；演示待补 |

**原则**：设备端保持轻量 UID 协议；一切「浏览器 / 国标 / OBS」放在 **网关进程**，
不要把 SIP/PS/WHIP 塞进 `p2p_natserver`。

---

## 7. 安全模型（P2P 摄像头的历史坑）

公开事故（综述与 CVE）：

- **iLnkP2P / PPPP**：硬编码密钥、可伪造 UID、可劫持任意摄像头。
- **TUTK 栈历史 CVE**：明文音视频、弱 UID 熵、云端指令未验源。

本仓库已避开的对应项：每 UID AuthKey、Connect Token + nonce、X25519 前向保密、
中继 HMAC、跨服 ICE_SDP HMAC。计划书安全红线必须保持：**禁止硬编码、禁止明文媒体、
禁止可预测 UID**。

---

## 8. 对本仓库的取舍（一句话）

| 模型 | 做 | 不做 |
|------|----|------|
| ICE + 自建 TURN | 已做，继续加固 | — |
| DERP 式「先通再切」 | P8：TCP/443 中继面 | 不做成系统 VPN / 虚拟网卡 |
| PCP/UPnP/NAT-PMP | 已落地（`PortMap`，失败忽略） | 续约/现网家宽验收待补 |
| 生日打洞 / NAT4E 预测 | P8 可选，默认关，有上限 | 不作为默认；对称×对称直接中继 |
| TCP 打洞 | P8 与 TLS 中继二选一或并行 | 不替代 UDP ICE |
| SFU / 多看客 | P9 旁路网关 | 不在设备上 mesh |
| WHIP/WHEP | P10 网关 | 不改 IOTC 主路径 |
| MoQ | P11 预研 | 不替代 1:1 预览 |
| 28181 | P9 网关 | 设备端不嵌国标栈 |
| libp2p / DHT | 只读论文 | 不做去中心化 |
| Iroh/QUIC 重写 | 不换底座 | UID 语义可对照 |

---

## 9. 参考链接

- [NVR P2P 方案对比（综述）](https://mickeyzzc.github.io/posts/nvr-p2p-solutions-comparison/)
- [P2P 信令与中继技术综述](https://blog.mickeyzzc.tech/en/posts/network/p2p-signaling-relay-survey/)
- [TUTK Kalay / P2P 远程看流](https://www.throughtek.com/p2p-iot-connection/)
- [libp2p DCUtR 测量（ProbeLab / IMC 2026）](https://blog.ipshipyard.com/dcutr-paper)
- [Iroh FAQ（vs Tailscale / libp2p）](https://www.iroh.computer/faq)
- [Tailscale DERP](https://tailscale.com/kb/1234/derp-servers) /
  [NAT traversal improvements](https://tailscale.com/blog/nat-traversal-improvements-pt-1)
- [EasyTier 打洞与 P2P 优化](https://easytier.rs/en/guide/network/p2p-optimize.html)
- [APNIC: How NAT traversal works](https://blog.apnic.net/2022/04/26/how-nat-traversal-works-nat-notes-for-nerds/)
- [PPPP / iLnk 协议族概述](https://sechub.in/view/3136036)
- [WHIP RFC 9725](https://www.rfc-editor.org/rfc/rfc9725.html) /
  [WHEP RFC 9737](https://datatracker.ietf.org/doc/rfc9737/)
- [webrtcHacks: WebRTC in 2026 / MoQ](https://webrtchacks.com/webrtc-in-2026-moq-whpp-and-the-end-of-the-sdp-era/)
- [Cloudflare Media over QUIC](https://blog.cloudflare.com/moq/)
- [GB28181 与 WebRTC 融合](https://cloud.baidu.com/article/3590164)
