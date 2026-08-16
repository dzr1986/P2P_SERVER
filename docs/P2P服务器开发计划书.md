# P2P 服务器开发计划书

> 更新：2026-08-16。本文是**活计划**：先写清现在有什么，再写还该做什么。
> 产品：可私有化部署、对标 TUTK Kalay 的 **有中心信令 IoT UID** 平台。
> 首要场景：IPC / NVR 远程预览与设备管理。**不是** EasyTier 式 SD-WAN。
>
> 编号约定：新工作只用 **档 A / B / C**。`P0–P11` 只在附录对照历史阶段，不再当待办清单。

---

## 0. 怎么用本文

| 你想… | 看 |
|--------|----|
| 现在做到哪、下一档是什么 | §2 |
| 进程怎么拆、流量怎么走 | §3 |
| 和 TUTK 差在哪 | §4 |
| 配置键 / 目录不能再堆什么 | §5 |
| 协议与选路（已落地） | §6 |
| **还该做什么、按什么优先级** | §7 |
| 绝对不能改什么 | §8 |
| 怎么验收、当前基线 | §9 |

配套（细节不在本文重复）：

| 文档 | 用途 |
|------|------|
| [`p2p_server优化完毕.md`](p2p_server优化完毕.md) | 信令核收口清单 |
| [`项目整体优化分析.md`](项目整体优化分析.md) | 全仓库体检与本轮改动 |
| [`EasyTier指南架构对照.md`](EasyTier指南架构对照.md) / [`EasyTier核心架构对照.md`](EasyTier核心架构对照.md) | 目录对照（只学编排） |
| [`EasyTier配置对照.md`](EasyTier配置对照.md) | 配置覆盖与键 |
| [`P2P模型图谱.md`](P2P模型图谱.md) | 学什么 / 不抄什么 |
| [`P2P穿透与流媒体实践.md`](P2P穿透与流媒体实践.md) | ICE / STUN / DERP / 通道 |
| [`启动与测试指南.md`](启动与测试指南.md) | 怎么跑、端口、`test.sh` |

---

## 1. 定位

### 1.1 对标什么

TUTK Kalay 的核心：设备烧一个 **UID**，全球客户端能连上；平台做 NAT 穿透和中继兜底；
SDK 提供 IOTC（会话）、AV（帧）、RDT（可靠流）、P2PTunnel（TCP 映射）。
直连成功则媒体不经服务器，省中继带宽。

本仓库对齐这条价值链，**不**对齐 Kalay 的云录像 / 推送 / App 账号。

### 1.2 目标（验收口径）

| 维度 | 目标 | 口径 |
|------|------|------|
| 功能 | UID 报到、LAN / P2P / Relay 自动选路、AV / RDT / Tunnel | **档 A 已具备** |
| 直连率 | 锥×锥 ≥ 85% | **不含** EDM×EDM（必须中继） |
| 可达率 | 含中继 ≥ 99.9% | 国内家宽多在 CGNAT 后，中继按 **15–30%（国内可按 ~30%）** 做容量 |
| 时延 | LAN < 300ms；打洞 < 3s；失败切中继总时长 < 8s | **档 B** 现网待验 |
| 规模 | 单 NatServer ≥ 10 万在线；单 Relay ≥ 500 Mbps | **档 B** 压测机待验，不是再改信令分发 |
| 部署 | 全部组件可私有化 | 裸机 / 容器均可 |
| 安全 | 每 UID AuthKey、连线 Token、AEAD + X25519 FS、中继 HMAC | **档 A 已具备** |

### 1.3 非目标

- 云存储、推送、账号云（独立立项）
- TUN / DHCP / 子网代理 / SOCKS / 魔法 DNS / mesh / 系统 VPN
- 把 SFU、28181、WHIP/WHEP、MoQ 塞进 `p2p_natserver`
- 改 `force_relay`「不打洞」语义（`test.sh` [2]/[11]/[21] 依赖）

---

## 2. 现在做到哪一档

一句话：**仓库内核已收口。** 下一档是现网验收和旁路网关，不是再拆 NatServer。

| 档 | 内容 | 状态 | 缺的是 |
|----|------|------|--------|
| **A. 仓库内核** | 信令、中继、UID/Token、IOTC/AV/RDT/Tunnel、ICE/DERP/TCP 打洞、同步/调度/状态口 | **已收口** | 无（小项见 §7.3） |
| **B. 现网验收** | 10 万心跳、弱网 1080p、家宽开孔、443 正式证书、三平台 SDK、唤醒出图 <6s、真 `tc netem` / iptables NAT 矩阵 | **待环境** | 机器、证书、真设备 |
| **C. 旁路网关** | 多看客 SFU、28181、WHIP/WHEP、MoQ 预研 | **未立项** | 独立进程，禁止进信令核 |

编排目标：**先保证通（中继/443），再升级直连**。一对多禁止设备侧 mesh。

---

## 3. 总体架构

```
                       ┌────────────── 管理面（档 B 补齐）──────────────┐
                       │  UID 批量签发 / 计费面    监控告警 / 部署手册   │
                       └──────┬───────────────────────┬────────────────┘
                              │                       │
        ┌─────────────────────▼───────────────────────▼─────────────────┐
        │              p2p_natserver 集群（只建连，不转发业务）            │
        │   区域 A: nat-a1 ↔ RegistrySync ↔ 区域 B: nat-b1 …             │
        │   报到 / 心跳 / CONNECT / STUN / REGION 调度 / 状态口            │
        └───────┬─────────────────────────────────────┬─────────────────┘
                │ 信令                                  │ RelayHealth
     ┌──────────▼─────────┐                ┌───────────▼──────────┐
     │  设备 / App SDK     │   直连成功      │  p2p_proxy 集群      │
     │  IOTC / AV / RDT    │◄═════════════► │  UDP 或 TCP/TLS 443  │
     │  Tunnel             │   失败才中继    │  HMAC + 配额         │
     └──────────▲─────────┘                └──────────▲───────────┘
                │ 离线目标                              │
                └──────── p2p_wakeserver POKE ──────────┘
```

```
设备/App ──心跳 / CONNECT──► p2p_natserver
                │
                ├─ 直连：LAN / ICE / TCP 打洞
                └─ 失败 ──► p2p_proxy（只转发）
离线目标 ──► p2p_wakeserver POKE
```

| 进程 | 职责 | 不做什么 |
|------|------|----------|
| `p2p_natserver` | 报到 / CONNECT / STUN / 同步 / 状态口 | 不转发 `MSG_PROXY_RELAY_DATA` |
| `p2p_proxy` | 数据面中继（DERP） | 不是 gateway / 不是 mesh 中转 |
| `p2p_wakeserver` | IoT 唤醒 POKE | 不替代信令 |
| `peer` / `iotc_demo` | 客户端演示 | — |
| `uidgen` / `tokengen` | 离线签发 | 计费面另立 |

二进制仍输出到 `server/natserver/bin/p2p_natserver`。

---

## 4. 术语对标

| TUTK | 本仓库落点 | 差距 |
|------|------------|------|
| UID（20 字符） | `core/foundation/Uid.*` + `tools/uidgen` | 批量签发与计费面待补（档 B） |
| P2P Server | `p2p_natserver`：心跳 / CONNECT / STUN / 同步 / REGION | 10 万心跳压测待补（档 B） |
| Relay | `p2p_proxy`：UDP + DERP TCP/TLS + HMAC + 配额 | 现网带宽压测、正式 443 证书（档 B） |
| IOTC SID / Channel | `client/sdk/iotc/IOTC.*`（128 SID，通道 0–31） | 已落地 |
| AVAPIs | `AVAPIs` + `AvCodec` + ABR/TWCC | 弱网 1080p 现网待补（档 B） |
| RDTAPIs | `RDTAPIs` | 大文件压测待补（档 B） |
| P2PTunnel | `P2PTunnel_Serve/Map` + `TunnelCodec` | ffmpeg/RTSP 演示待补（档 B） |
| LAN Search | `239.255.77.89:17890` | 已落地（`P2P_DISABLE_LAN=1` 可关） |
| Device Wakeup | `p2p_wakeserver` | 三平台接入与 <6s 出图待补（档 B） |
| AuthKey / Token | 每 UID AuthKey + `ConnectAuth` + nonce 防重放 + X25519 FS | 已落地 |

旧路径 `common/*.h`、`server/natserver/src/*.h`、`client/sdk/{plat,proto,session,transport,api}/*.h` 只是转发头，逻辑在 `core/` 与 `server/`。

---

## 5. 目录与配置

```
server/
  instance/       生命周期组合根（只焊，不堆同步/择优/验签实现）
  config/         文件 < 已设置的 P2P_* < 命令行
  peers/          PeerManage + RegistrySync
  connectivity/   STUN responder / 映射观察 / NAT 探测
  management/     AntiAbuse、JailFile、License、AuthChallenge、ConnectAuth、RelayHealth、Status
  rpc/            on_msg_*
  listener/       bind + LocalListeners 防回环
  proxyserver/    专用中继
  wakeserver/     唤醒
core/
  foundation → socket → packet → tunnel → connectivity → instance
```

`instance` 不得再长出同步协议或代理择优。不建 `gateway/`、`peers/route`、`wasi/`。

配置覆盖（学 EasyTier）：`P2pServers.cfg` < 已设置的 `P2P_*` < 命令行。
文件值支持 `${ENV}`；`P2P_DISABLE_ENV_PARSING=1` 只关展开。

| 键 | 作用 |
|----|------|
| `PrivateMode` / `P2P_PRIVATE_MODE` | 强制 `EnableAuth`（仍需 `AuthSecret`） |
| `StatusAllow` / `P2P_STATUS_ALLOW` | 状态口 CIDR；空=不限制 |
| `JailFile` / `P2P_JAIL_FILE` | fail2ban：一行一个拉黑 IP |
| `P2P_PROXY_AUTH_SECRET` | 中继注册 HMAC；空=演示默认（测试兼容） |
| `P2P_NEED_P2P` / `peer -need-p2p` | 本端尽快直连，心跳宣告 `np=1` |
| `force_relay` / `peer -relay` | **硬关打洞**；对端 `need_p2p` 也不能打开 |

---

## 6. 核心设计（档 A 已落地）

### 6.1 UID

```
UID(20) = PREFIX(4) + REGION(1) + RANDOM(12) + CRC(3)   // Base32
```

- `uidgen` 离线批量签发 `UID + AuthKey`；服务端用 `AuthSecret` 派生，不存明文 AuthKey。
- `UidStrict=1` 拒绝非结构化 UID。
- 连线 Token：`tokengen` 签发；`EnableConnectToken=1` 时 CONNECT 尾部验签 + `TokenNonceCache` 防重放。实现在 `server/management/ConnectAuth.h`。

### 6.2 通道

```
IOTC_Session (SID ≤ 128)
 ├─ ch0  IOCtrl（可靠）
 ├─ ch1  AV 视频（不可靠 + FEC + too-late-drop）
 ├─ ch2  AV 音频
 ├─ ch3  RDT 文件（可靠 + 背压）
 └─ chN  P2PTunnel（RTSP/HTTP/SSH）
```

C API 形态见 `client/sdk/iotc/`（`IOTC_*` / `av*` / `RDT_*` / `P2PTunnel_*`）。

### 6.3 连线

```
LAN 组播 → CONNECT(token) → ACK/INVITE（公网/LAN/NAT/Relay 候选 + punch hint）
  → 发起方 ICE gather（controlling）→ 直连
  → 失败或 force_relay → Proxy（UDP 或 TCP/TLS）
  → 非 force_relay 时后台继续打洞，direct_ok 无缝切回
```

CONNECT 尾部 1 字节 hint（旧客户端忽略）：

| hint | 含义 |
|------|------|
| 0 | 旧服务端 / 未知 |
| 1 Ice | 锥×锥、锥×对称：满超时打洞 |
| 2 Relay | EDM×EDM：仍打洞，但 1.5s 后开中继 |
| 3 Need | 任一侧心跳 `np=1`：满超时打洞；**不能**盖掉 Relay，也**不能**打开 `force_relay` |

选路：`PathSelect`（ICE nominated → juice_prev → juice+direct_ok → TCP 打洞 → UDP 打洞 → DERP TCP → UDP 中继）。

### 6.4 安全

| 层 | 现状 |
|----|------|
| 报到 | 挑战应答；每 UID AuthKey；失败走 AntiAbuse |
| 连线 | ConnectToken + nonce |
| 隧道 | AEAD；X25519 FS 握手（PSK 不进入 FS 密钥）；180s 轮换不断流 |
| 集群 | RegistrySync HMAC；hop 防环 |
| 中继 | `ProxyAuth` HMAC + 源地址校验 + 每小时配额窗口 |
| 管理 | 黑名单必须 `AdminSecret`；统计口有密钥才校验 HMAC |
| 主机 | `JailFile` 给 fail2ban |

### 6.5 唤醒

设备低频 `MSG_WAKE_KEEPALIVE`（可选 `WakeSecret`）。CONNECT 目标离线时 NatServer 通知 wakeserver `TRIGGER`，向上次公网地址 `POKE`。Trigger 本身尚无 HMAC（协议扩字段需兼容期，见 §7.3）。

---

## 7. 下一步（按优先级）

不再把历史阶段写成「待做说明书」。changelog 见 git 与收口文档。

### 7.1 档 B — 现网验收（优先）

这些**不改协议**也能做，缺的是机器、证书和真设备。按依赖排序：

| 优先级 | 项 | 验收 | 依赖 |
|--------|----|------|------|
| B1 | 部署手册：裸机 / 容器 + 正式 443 证书 | 企业只放 443/TCP 时 <8s 出图 | 证书、公网机 |
| B2 | 单节点 10 万心跳、Relay 吞吐打流 | 对照 §1.2 规模目标 | 压测机 |
| B3 | 分 NAT 类型埋点（锥与对称分母分开） | 直连率口径可报 | 现网或 docker+iptables |
| B4 | 家宽 IGD 开孔续约 | 锥型家宽直连率可测 | 真路由；CI 默认 `P2P_DISABLE_PORTMAP=1` |
| B5 | `tc netem` 弱网 1080p | P50/P99、卡顿；无权限时继续用户态丢包 | 网络命名空间 |
| B6 | 唤醒到出图 <6s | 真低功耗设备 | 设备 + wakeserver |
| B7 | Android / iOS / Windows SDK 包 | `Plat.h` 已隔离，差打包 | 交叉编译链 |
| B8 | ffmpeg 经 Tunnel 拉 RTSP、≥1GB RDT | 演示与大文件校验 | 演示机 |

本 CI 无 iptables/`ip`，`unshare` 被拒——B3/B4/B5 **不算内核未完成**。

### 7.2 档 C — 旁路立项（原 P9–P11）

**一律独立进程，不进 `p2p_natserver`。** 档 B 未灰度前不要开工。

| 编号 | 内容 | 验收 |
|------|------|------|
| C1（原 P9） | 多看客走 SFU/转推（go2rtc / mediasoup / ZLMediaKit 之一）；可选 28181 网关 | 3 路同看时设备上行不随看客数涨；设备端不重写国标栈 |
| C2（原 P10） | 网关 WHIP/WHEP，浏览器预览不强制 IOTC | Chrome 能看已连 UID；不替换 juice+自研帧 |
| C3（原 P11） | MoQ 调研笔记 | 不替代 1:1 预览，不改核心协议 |

不做：MCU 混流、HLS/CDN 回看、把 SIP/WHIP 塞进 NatServer。

### 7.3 仓库内小项（不挡现网）

来自 [`项目整体优化分析.md`](项目整体优化分析.md)，有空再做：

1. Proxy TCP 会话可 join 的优雅停机
2. WakeTrigger 可选 HMAC（旧空字段兼容）
3. 心跳压测器 / NAT 矩阵脚本接到独立 CI 机

### 7.4 关系

```
档 A 仓库内核 ──已收口──► 档 B 现网灰度 ──► GA
                         │
                         └─► 档 C 旁路网关（C1 → C2，C3 只调研）
```

---

## 8. 红线

| 红线 | 原因 |
|------|------|
| 不改 `force_relay` | [2]/[11]/[21]：硬关打洞；NEED 也不能打开 |
| juice 状态/recv 回调里不调任何 `juice_*` | 与 `mu_` 死锁 |
| 持 `mu_` 的 tick 里不查 juice | 同上；restart 完成探测必须锁外 `juice_get_state` |
| `juice_set_remote_gathering_done` 仍在 tick 持锁 | 锁外会与回调并发，[3]/[5] 单边 CONNECTED |
| RegistrySync 三条日志原文不动 | [6]/[8] grep |
| `p2p_node_region` 不塞 instance 标签 | [12] |
| NatServer 不转发 `MSG_PROXY_RELAY_DATA` | 信令核只建连 |
| 不抄 TUN / mesh / `--p2p-only` / gateway | 产品不是 SD-WAN；IoT 必须能中继 |

---

## 9. 测试与基线

| 层 | 手段 | 说明 |
|----|------|------|
| 单测 | `tests/bin/*`（[0] 挂上） | PunchAdmit / PunchPolicy / CfgFile / server_arch / ConnectAuth |
| 端到端 | `test.sh` [0]–[21] | 数字以脚本为准；cleanup 的 `Killed` 不是失败 |
| 弱网 | 用户态切片丢失；真 `tc netem` 待环境 | 档 B5 |
| NAT 矩阵 | `NatSim` + `nat_matrix.sh`；docker+iptables 待环境 | 档 B3 |
| 规模 / 现网 | 档 B1–B8 | |

**当前 CI 基线**（2026-08-16）：`test.sh` **PASS=139 FAIL=1**。[16] ICE restart 已过；剩余 `G FS handshake missing` 是 [5] 7s 窗口内 X25519 日志偶发，与协议改动无关。

本环境：无 `killall`（`pkill -f` + `[x]`）；不要在同一条 shell 里同时写 `peer`/`iotc_demo` 路径和 `pkill -f`。
`P2P_DISABLE_PORTMAP=1` 是 CI 默认（无家宽 IGD）。

RegistrySync 日志必须仍含：`registry sync enabled, %zu peer(s)`、`sync entry uuid[%s]`、`sync snapshot sent`。

---

## 10. 风险

| 风险 | 对策 |
|------|------|
| 国内 CGNAT / 对称 NAT 多 | 锥与对称分母分开；中继按 ~30%；EDM×EDM 走 Relay hint |
| 企业只放 TCP/443 | DERP 已落地；现网绑 443 + 正式证书（B1） |
| 多看客 mesh | 档 C 旁路网关；设备只出一份码流 |
| 生日打洞触发风控 | 默认关；N 上限、间隔、熔断 |
| 嵌软 RAM 紧 | `Plat.h` 裁剪（去 ICE/减缓冲） |
| 自研握手 | X25519 有向量单测；AEAD 复用已测路径；外部评审仍建议做 |
| 历史 P2P 厂商 CVE | 每 UID 密钥、Token nonce、FS、中继 HMAC；红线保持 |
| 把 MoQ/28181/WHIP 塞进核心 | 全部进独立网关；1:1 IOTC 主路径冻结 |

---

## 11. 交付物

**已有（档 A）**

1. `p2p_natserver` / `p2p_proxy` / `p2p_wakeserver`
2. Linux 上的 IOTC/AV/RDT/Tunnel SDK + `peer` / `iotc_demo`
3. `uidgen` / `tokengen`、`test.sh`、分层文档

**待环境（档 B）**

4. 部署手册（Docker/裸机 + 443 证书）
5. 压测器、现网穿透率（锥/对称分开）
6. 三平台 SDK 包

**待旁路（档 C）**

7. SFU / 28181 / WHIP 网关进程与 MoQ 笔记 —— **不进 NatServer**

---

## 附录：历史阶段编号对照

只为读旧文档 / 旧提交。新计划用档 A/B/C。

| 旧编号 | 交付 | 回归 | 现归属 |
|--------|------|------|--------|
| P0 底座 | 打洞 / 中继 / 鉴权 / AEAD / ICE / Session | `test.sh` [0][1][2] | 档 A |
| P1 身份 | 结构化 UID、每 UID AuthKey、ConnectToken | [9][14] | 档 A |
| P2–P4 通道 | IOTC / AV / RDT / Tunnel；中继四通道 | [10][11] | 档 A |
| P5 安全 | X25519 FS、中继配额 | [5] | 档 A |
| P6 集群 | RegistrySync、REGION 调度、`GET /` + `/metrics` | [6][8][12] | 档 A |
| P7 唤醒 | wakeserver 协议 | [13] | 档 A |
| P8 穿透 | PortMap、NAT 矩阵、need_p2p、DERP、TCP 打洞、PunchAdmit/PunchPolicy、PathSelect | [15]–[21] | 档 A（现网家宽/iptables 在档 B） |
| P9 | 多看客 SFU / 可选 28181 | — | 档 C1 |
| P10 | WHIP/WHEP 网关 | — | 档 C2 |
| P11 | MoQ 调研 | — | 档 C3 |

P8 服务端半边另含：`need_p2p` 跨对端宣告、`PrivateMode`、`StatusAllow`、`JailFile`、NatServer 丢弃 `MSG_PROXY_RELAY_DATA`。
