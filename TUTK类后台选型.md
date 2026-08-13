# 做 TUTK 类后台，该选哪个二次开发

**结论先说：连通层选 iroh，不要选 libp2p / libtorrent。**

如果你的产品还要做摄像头预览、浏览器播放、协议互转，媒体层再叠加 **ZLMediaKit**（或 Pion WebRTC + coturn）。  
TUTK 的 Master（UID、鉴权、计费、区域调度）没有现成开源对标，这一层必须自研。

---

## TUTK 后台到底是什么

TUTK（ThroughTek / Kalay）不是 BT，也不是 IPFS。它是一套 **IoT 设备连通平台**：

| TUTK 模块 | 作用 |
|---|---|
| **UID** | 设备唯一 ID，APP 靠它找设备 |
| **Master** | 校验 UID、管理 P2P 服务器、按区域调度 |
| **P2P Server** | 设备报到 / 心跳、协助 APP 连设备、打不通就转发 |
| **IOTC** | 建连 + 不可靠通道（SID） |
| **RDT** | 可靠数据通道 |
| **AV** | 音视频封装 |
| **P2PTunnel** | 把 HTTP / RTSP / SSH 等 TCP 协议套进隧道 |

官网说明见：[TUTK P2P 与数据传输](https://www.throughtek.cn/help-p2pConnection/)

所以你要二次开发的「类似 TUTK 的后台」，核心是三件事：

1. **用 ID 找到在线设备**（报到、心跳、鉴权）
2. **尽量打洞直连**（省带宽、降延迟）
3. **打不通就中继**（保证能看、能控）

上一份清单里的 libtorrent、Syncthing、Kubo 对这三件事都不对口。

---

## 和开源栈的对应关系

| TUTK | 最接近的开源选择 | 要不要自己写 |
|---|---|---|
| UID | iroh 的 EndpointId（公钥） | 业务 UID ↔ 公钥 的映射要自研 |
| Master | 无对标 | **必须自研** |
| P2P Server（打洞 + 中继） | **iroh-relay** | 可二次开发、可私有化部署 |
| IOTC 建连 | iroh Endpoint「按公钥拨号」 | 设备 / APP SDK 用 iroh |
| RDT 可靠通道 | QUIC stream | 上层业务协议自研 |
| AV | 不包含 | 自研，或走 ZLMediaKit / WebRTC |
| P2PTunnel | iroh 上套 TCP 隧道，或 dumbpipe 思路 | 要自研封装 |

---

## 选型：只选一个连通栈的话，选 iroh

仓库：<https://github.com/n0-computer/iroh>  
IoT 说明：<https://www.iroh.computer/solutions/iot>  
中继概念：<https://docs.iroh.computer/concepts/nat-traversal>

### 为什么是它

1. **按 ID 拨号，不是按 IP 拨号。** 这和 TUTK「APP 填 UID 就能连设备」是同一类产品模型。
2. **打洞是一等能力，中继是兜底。** 官方量级大约 9/10 能直连，失败自动走 relay。这正是 TUTK 省服务器成本的逻辑。
3. **relay 可以私有化。** `iroh-relay` 就在同一个仓库里，线上公共中继也是这份代码。对应 TUTK 可私有化部署的 P2P Server。
4. **明确面向 IoT。** 同一套 API 覆盖 Linux SBC、树莓派，也有嵌入式方向。
5. **你把时间花在业务协议上，而不是先搭 NAT 迷宫。** libp2p 要自己拼 AutoNAT、DCUtR、Circuit Relay、多传输协商，TUTK 类产品用不上那么多。

### 它缺什么（必须心里有数）

- **没有 Master。** 账号、License、UID 校验、区域、计费、踢设备，都要自己做。
- **没有现成 AV API。** 它给的是加密 QUIC 管道，码流、I 帧、对讲要自己定义，或接到媒体服务器。
- **浏览器不能直接跑 iroh。** 它走 QUIC/UDP。网页预览要加网关，或媒体层改走 WebRTC。
- **语言是 Rust。** 设备端 / 服务端二次开发要能接受 Rust；其他语言靠 FFI。
- **纯 UDP 被墙的网络只能走中继。** 这和 TUTK 也类似，不是缺陷，但中继容量要按峰值算。

---

## 不要选什么

| 项目 | 为什么不适合做 TUTK 后台 |
|---|---|
| **libp2p** | 协议全集很强，但 NAT 打洞成功率、配置复杂度都不如 iroh。DHT / PubSub 不是 TUTK 的核心。 |
| **libtorrent** | BT 分发引擎，没有「设备 UID 长连接 + 中继调度」。 |
| **Syncthing** | 文件夹同步产品，不是连通平台。 |
| **Kubo / IPFS** | 内容寻址，不是设备寻址。 |
| **单独一个 coturn** | 只有 TURN，没有 UID 报到、没有设备 SDK、没有 Master。 |

libp2p 可以当备选，但只建议这两种情况再用：

- 你已经有 Go/JS 团队，且必须进浏览器（js-libp2p WebRTC）
- 你明确需要 DHT、PubSub、多传输协商

即便如此，TUTK 类「UID → 连上那台摄像头」这条主路径，iroh 更贴。

---

## 如果产品是摄像头，建议两层，不要只抱一个库

TUTK 把「连通」和「音视频」绑在一个 SDK 里。开源世界这两层最好拆开。

```
APP / 网页
    │
    ├─ 控制、信令、UID ──► 自研 Master
    │                         │
    │                         ▼
    ├─ 尽量直连 ────────► iroh-relay 集群（打洞失败才转发）
    │
    └─ 预览 / 回放 / 对讲 ─► ZLMediaKit 或 Pion WebRTC
                              （浏览器、RTSP、录像走这里）
```

### 媒体层怎么选

| 需求 | 选 |
|---|---|
| 国内摄像头、RTSP / GB28181 / 协议互转、要尽快有预览 | **[ZLMediaKit](https://github.com/ZLMediaKit/ZLMediaKit)** |
| APP / 浏览器标准 WebRTC，信令自己控 | **[Pion WebRTC](https://github.com/pion/webrtc)** + [coturn](https://github.com/coturn/coturn) |
| 只要设备 ↔ APP 私有码流，不要浏览器 | 直接在 iroh QUIC stream 上自研 AV（最接近 TUTK AV API） |

ZLMediaKit 自带信令、STUN/TURN、WebRTC P2P/SFU，很适合「先把流跑起来」。  
它更像 **流媒体中台**，不是 TUTK 那种「百万设备报到、按 UID 调度 P2P 节点」的 Master。设备登录、鉴权、License 还是要自研。

---

## 推荐落地顺序

### 第一期：先做出「能按 ID 连上」

1. 自研 Master：设备注册、UID、token、在线心跳、区域。
2. 部署私有 **iroh-relay**（先单机，再多区域）。
3. 设备端、APP 端都接 iroh Endpoint：APP 拨设备的 EndpointId。
4. 先跑通控制通道（开关、PTZ、配置），不要一上来就上 4K。

### 第二期：补齐 TUTK 产品能力

5. 在 QUIC stream 上做可靠/不可靠通道划分（对标 IOTC / RDT）。
6. 音视频：私有码流走 iroh，或设备推到 ZLMediaKit，APP/网页拉 WebRTC。
7. 需要的话做 TCP 隧道（对标 P2PTunnel，套 RTSP/HTTP）。
8. 中继计量、限速、按 UID 计费——这才是后台赚钱的部分，开源库不会给你。

### 不要一上来就做的

- 不要先改 libp2p 源码「做成 TUTK」。
- 不要指望某个开源库带 License / UID 授权。
- 不要用公网免费 relay 当生产：限速、无 SLA、数据面不在你手里。

---

## 一句话对照

| 你的真实目标 | 二次开发主体 |
|---|---|
| 替代 TUTK 的 **IOTC + P2P Server**（UID 连通） | **iroh / iroh-relay** |
| 替代 TUTK 的 **Master**（账号、授权、调度） | **自研** |
| 替代 TUTK 的 **AV / 预览** | **ZLMediaKit** 或 **Pion**，或 iroh 上自研 |
| 只做网页看直播、不强调设备 P2P | 直接 ZLMediaKit，不必上 iroh |

**最终建议：连通层二次开发 iroh，业务后台自研 Master，摄像头预览按需加 ZLMediaKit。**
