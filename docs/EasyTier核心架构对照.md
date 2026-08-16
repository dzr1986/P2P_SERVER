# EasyTier-core 目录对照（只学编排，不抄成 SD-WAN）

> 对照源：本机只读树 `/home/ubuntu/EasyTier/easytier-core/src/`（**不要** `git add` 进本仓库）。
> 配套：[`P2P模型图谱.md`](P2P模型图谱.md) §4.5、[`开发指南.md`](开发指南.md) §1、[`P2P服务器开发计划书.md`](P2P服务器开发计划书.md) P8。

EasyTier 是 **开源 SD-WAN / 虚拟网卡 mesh**。本仓库是 **有中心信令的 IoT UID 会话**。
分层可以学，产品形态不能抄。

---

## 1. 他们的依赖方向（自下而上）

```
foundation          无网络域依赖：task / time / stats / token_bucket
    ↓
socket              TCP / UDP / ring（在 tunnel 之下）
    ↓
packet              ZCPacket、打洞包、STUN / TCP / UDP 头
    ↓
tunnel              加密 + framed；把 socket 升级成可转发通道
    ↓
connectivity        stun / hole_punch / direct / protocol / transport
    ↓
peers               peer_manager / route / ACL   ← 本仓库不抄 mesh 路由
    ↓
listener            accept + RunningListenerRegistry
    ↓
instance            生命周期组合（把上面全焊在一起）
    ↓
events.rs           CoreEvent 通知 Host
host / wasi         DNS、环境、WASI 缝
config / rpc / management   配置与管理面
gateway             SOCKS / smoltcp / VPN 门户  ← 明确不抄
```

`lib.rs` 只做 `pub mod` 声明，领域逻辑在各目录。`CONTEXT.md` 写明：
**foundation 不得依赖任何网络域；上层可任意用 foundation。**

---

## 2. 逐目录：学什么 / 不学什么 / 本仓库落点

| EasyTier 目录 | 职责（一句话） | 本仓库学 | 本仓库不学 | 落点 |
|---------------|----------------|----------|------------|------|
| `foundation/` | 时钟、任务、限速、统计，无业务 | 分层：平台能力不夹 NAT | operation_broker 整套异步 ID | `Plat.h`、`common/` 无域头文件 |
| `socket/` | 已建立/可 bind 的端点 | UDP/TCP 封装在 tunnel 之下 | ring / 进程内回环当产品 | `UdpSocket.h`、`TcpPunch.h`、`TlsIo` |
| `packet/` | 零拷贝包 + 打洞/STUN 头 | 控制面与数据面分魔数 | ZCPacket 喂 TUN/NIC | `ProtoDef.h`、`StunBind.h`、`Codec.h` |
| `tunnel/` | socket → 加密通道 | 帧头明文、负载 AEAD | WireGuard / QUIC 隧道族 | `Session.h` + `encrypt_tunnel_frame` |
| `connectivity/` | **连接编排**（最值得学） | STUN、打洞、直连、协议升级、路径准入 | 多 scheme URL 拨号、WG/QUIC | 见 §3 |
| `peers/` | 多对等体 + 路由 + ACL | 每 UID 一条 Conn | **mesh 为别人转发**、DHT | `P2PClient::conns_` |
| `listener/` | 监听 + 防回环拨自己 | 本机 listener URL 不 hairpin | 多 scheme 监听计划 | 信令 UDP + ICE host；回环不配 STUN |
| `instance/` | 生命周期组合 | 一个门面焊齐 STUN/打洞/中继 | 数据面扩展 / VPN portal | `P2PClient` |
| `events.rs` | 核心 → Host 通知 | 回调契约（勿在回调里重入） | 对等体增删当 VPN 事件 | `on_ready` / `on_connected` |
| `host/` | DNS、环境、能力缝 | 平台差异外置 | WASI Host | `Plat.h` |
| `config/` | TOML + 运行时配置 | 文件 < 环境 < 命令行；`${ENV}`；状态口白名单 | 网关/浏览器配置整棵 | `CfgFile` + `P2PClient::Config`（见 [EasyTier配置对照.md](EasyTier配置对照.md)） |
| `rpc/` `management/` | 管理 RPC | 运维口与数据面分离 | 把管理面做成平台 | NatServer Status / 管理报文 |
| `gateway/` | SOCKS / smoltcp / 端口转发 | — | **整目录不抄** | 将来旁路网关进程，不进 NatServer |
| `wasi/` | WASM 适配 | — | 不抄 | — |
| `process_runtime.rs` | 进程内 runtime | 单线程 worker + tick | tokio 运行时重写 | `P2PClient` worker |

---

## 3. `connectivity/` 内部（本仓库要对齐的那一层）

EasyTier 把「怎么连上」从「连上之后怎么转发」拆开：

| 子模块 | 他们做什么 | 本仓库对应 |
|--------|------------|------------|
| `stun/` | UDP/TCP STUN 采集映射；`responder` 按 CHANGE-REQUEST 换口回 | `NatDetect` + `StunResponder`（备用口）+ TCP STUN + `StunBind.h` |
| `hole_punch/udp` | UDP 打洞引擎 | libjuice ICE + 自研 punch socks |
| `hole_punch/tcp` | 同时 connect | `TcpPunch.h`（默认关，EDM 不发起） |
| `hole_punch/port_mapping` | UPnP → NAT-PMP → PCP | `PortMap.*` |
| `hole_punch/policy` | `should_try_p2p` / `should_background_p2p`、序列 BackOff | `PunchPolicy.h` + `force_relay`（硬关）/ `lazy_p2p` / `want_direct` |
| `direct/` | 已知公网口直拨 | CONNECT 下发的 `direct` / LAN |
| `transport/` | TCP/UDP 端点，`first_success` 竞速 | **`PathSelect.h`**（发送准入，不竞速拨号） |
| `protocol/` | 端点升级成 tunnel（tcp/udp/wg/quic） | 隧道帧加密；不升级成 WG |
| `manual/` | 配置里写死的对端 URL | 本仓库用 UID，不用 URL 拨号 |
| `LocalListenerUrls` | 禁止拨自己的监听地址 | 回环 `bind_address=127.0.0.1`、不配 STUN |

**学到的一条规则**：策略函数只吃「事实结构体」，不持锁、不调传输 API。
所以 `PathSelect` / `NatMatrix` 都是头文件纯函数；`juice_*` 只许在 worker tick 里、且不在 juice 回调里调用。

---

## 4. 本仓库已拆成的目录（对照上面）

物理树在 [`core/`](../core/README.md)，`ls core` 即可按层学：

```
core/
  foundation/                 Plat Crypto Uid Token Handshake Log File Util
  socket/                     Net UdpSocket TlsIo Packet
  packet/                     ProtoDef Codec StunBind IceSdp
  tunnel/                     Session AbrEstimate TwccEstimate
  connectivity/
    stun/                     NatDetect
    hole_punch/               TcpPunch PortMap NatMatrix NatSim
    transport/                PathSelect
  instance/                   P2PClient（生命周期 + 选路发送）
client/sdk/iotc/              对外 UID API（TUTK 形，不是 EasyTier 层）
server/natserver|proxyserver  中心信令 + 专用中继（他们没有对等物）
```

`common/*.h` 与旧 `client/sdk/{plat,proto,session,transport,api}` 只是兼容转发头。

**p2p_server（NatServer）** 已按同样模块名拆到 `server/`：

| EasyTier | 本仓库 p2p_server |
|----------|-------------------|
| `config/` | `server/config/CfgFile.*` |
| `peers/` | `server/peers/PeerManage.*`（仅注册表） |
| `connectivity/` | `server/connectivity/` STUN responder + 映射观察 + NAT 探测 |
| `management/` | `server/management/` 防滥用、白名单、Status |
| `rpc/` | `server/rpc/RecvProcess.cpp` |
| `instance/` | `server/instance/NatServer.*` |
| `listener/` | `server/listener/` UDP bind + LocalListeners 防 hairpin |
| `hole_punch/policy` | CONNECT `PunchAdmit` + 客户端 `PunchPolicy`：序列 BackOff、lazy 后台打洞 |
| `stun/responder` | CHANGE-REQUEST 从备用口回；OTHER-ADDRESS 广告备口 |
| `stun/collector` | `MappingObserve`：主/备口映射补 CONNECT nattype |
| `gateway/` | **不建**；中继是独立进程 `proxyserver/` |

`send_tunnel_via` 用 `PathSelect` 决定走哪条已就绪路径：

`ICE nominated → juice_prev → juice+direct_ok → TCP 打洞 → UDP 打洞 → DERP TCP → UDP 中继`

`force_relay` 仍跳过全部直连族（测试 [2]/[11]）。

---

## 5. 明确禁止从这些目录长出来的功能

- `gateway/`：SOCKS、smoltcp 用户态栈、VPN 门户、端口转发当产品
- `peers/route`：节点为别人转发、子网交换、foreign network
- `packet` 喂 TUN：本仓库没有虚拟网卡
- `wasi/`、把 `management` RPC 做成控制平台
- 多 scheme（`wg://` `quic://` `ws://`）当 IPC 主路径

只许继续加：**打洞策略、中继面、NAT 矩阵、路径准入、家宽开孔**。
一对多观看 / 28181 / WHIP 走独立网关进程，见计划书 P9–P10。
