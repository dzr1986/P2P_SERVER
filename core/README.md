# core/ — 按 EasyTier-core 目录拆的可学习分层

对照 EasyTier `easytier-core/src/` 的 `lib.rs` 模块表。本目录是 **C++ 编排层**，
不是 SD-WAN。依赖只允许向上：

```
foundation → socket → packet → tunnel → connectivity → instance
```

`ls core` 时按层读，不要按文件名平铺搜。

**不建这些目录**：`gateway/`（SOCKS/VPN）、`peers/route`（mesh 转发）、`wasi/`。
对等体表在 `instance` 的 `Conn` map 里，不为别人转发。

旧路径 `common/*.h`、`client/sdk/{plat,proto,session,transport,api}/*.h`
仍是兼容头，只 `#include` 到这里，勿再往旧文件加逻辑。

| 目录 | EasyTier 对等 | 这里有什么 | 禁止依赖 |
|------|---------------|------------|----------|
| `foundation/` | `foundation/` | 时钟 `Plat`、日志、文件、Crypto、UID、Token、握手、区域排序 | 不要 include `socket/` 以上（Uid/Token 目前读 `packet/ProtoDef` 长度常量，不再扩大） |
| `socket/` | `socket/` | `Net`/`UdpSocket`/`TlsIo`/`Packet` 读写器 | 不要 include `connectivity/` / `instance/` |
| `packet/` | `packet/` | `ProtoDef`、`Codec`、STUN Binding、ICE SDP | 不要 include `tunnel/` 以上 |
| `tunnel/` | `tunnel/` | `Session` 可靠帧、ABR/TWCC | 不要 include `connectivity/` / `instance/` |
| `connectivity/stun/` | `connectivity/stun` | 客户端 NAT 自检 | 不要调 `juice_*` |
| `connectivity/hole_punch/` | `hole_punch/*` | TCP 打洞、开孔、NAT 矩阵/仿真 | 策略纯函数，不持锁 |
| `connectivity/transport/` | `transport` | `PathSelect` 发送准入 | 不碰 socket / juice |
| `instance/` | `instance/` | `P2PClient` 生命周期 + worker | 对外门面；juice 只在 tick 里调 |

对外 UID API 仍在 `client/sdk/iotc/`（TUTK 形，不是 EasyTier 层）。
**p2p_server 自己的拆分**在 [`server/README.md`](../server/README.md)：`config / peers / connectivity / management / rpc / listener / instance`。
