# p2p_server 优化完毕

> 范围：本仓库的 **NatServer 信令核**（`p2p_natserver`）以及它依赖的打洞策略 / 配置 / 状态口。
> 产品仍是「有中心信令的 IoT UID」，不是 EasyTier SD-WAN。
> **不要改 `force_relay` 语义**（测试 [2]/[11]/[21] 依赖「不打洞」）。

本文把「还能在 p2p_server 里做完的事」和「环境做不了 / 明确旁路」分开写，避免把网关、压测机、三平台 SDK 算成信令核未完成。

---

## 1. 结论

**p2p_server 信令核已收口。** 计划书 **档 A**（历史 P0–P8 仓库内核）里属于 NatServer / 打洞策略 / 配置 / 观测的部分都已落地。
档 B 现网、档 C 旁路（原 P9–P11）、10 万心跳、现网 NAT 矩阵、三平台 SDK、真 `tc netem` **不算 p2p_server 未完成**。

| 档 | 状态 |
|----|------|
| P0–P1 信令 / UID / Token / 鉴权 | 已落地；Token 验签抽出到 `ConnectAuth` |
| P2–P4 IOTC / AV / RDT / Tunnel | 客户端 SDK，不在 p2p_server 再堆 |
| P5–P6 同步 / 状态 / 区域调度 | RegistrySync + Status JSON/metrics |
| P7 唤醒 | 独立 `p2p_wakeserver` |
| P8 穿透策略（服务端半边） | PunchAdmit / PunchPolicy / STUN / TCP STUN / PathSelect |
| P8 现网家宽 / iptables 矩阵 | **环境做不了** |
| P9–P11 SFU / 28181 / WHIP / MoQ | **旁路网关，禁止进 `p2p_natserver`** |

---

## 2. 本轮收口（相对「只做本端 need_p2p」）

### 2.1 跨对端 `need_p2p`

学 EasyTier `--need-p2p`，但走 **心跳 extinfo**，不改 CONNECT 定长头（旧节点当不透明串）。

| 步骤 | 落点 |
|------|------|
| 客户端心跳 | `P2PClient::do_heartbeat`：`need_p2p` 时带 ASCII `np=1`（明文与加密心跳都带） |
| 注册表 | `Peer.need_p2p`；`upsert` / `upsert_synced` 用 `extinfo_has_need_p2p` |
| CONNECT | 任一侧 `need_p2p` 且准入不是 Relay → hint=`PUNCH_HINT_NEED=3` |
| 客户端 ACK/INVITE | `hint==NEED` 置 `want_direct`；等待按 Ice（满超时） |
| EDM×EDM | 仍 `PUNCH_HINT_RELAY`，NEED **不能**盖掉 |
| `force_relay` | **更硬**：NEED 也不能打开打洞 |

### 2.2 CONNECT Token 抽出

`server/management/ConnectAuth.h`：`verify_connect_trailer`。
密码学仍在 `core/foundation/ConnectToken.h`。`NatServer` 只做薄包装。

### 2.3 fail2ban 导出

学 guide `network/host-public-server.md` 的主机加固思路，不抄他们的 systemd 单元。

- 配置：`JailFile=` / `P2P_JAIL_FILE`
- `AntiAbuse::dump_jail()`：当前未过期拉黑 IP，**一行一个**
- 黑名单有变更时（timer 落盘）同步写 jail 文件

### 2.4 状态口

`GET /` JSON 增加：`private_mode`、`sync`、`flood_drop`、`need_p2p`、`relay_reject`。

`GET /metrics` 增加：`p2p_private_mode`、`p2p_registry_sync`、`p2p_need_p2p_peers`、
`p2p_flood_drop_total`、`p2p_relay_reject_total`。

`p2p_node_region` **不**再塞 instance 标签（测试 [12]）。

### 2.5 信令核不转发业务

`MSG_PROXY_RELAY_DATA` 进 NatServer 直接丢，计数 `relay_reject`。
业务只走 `p2p_proxy`。对齐 guide「共享节点只帮建连」。

---

## 3. 目录（不要再把实现堆回 NatServer.cpp）

```
server/
  instance/       生命周期组合根
  config/         文件 < P2P_* < 命令行
  peers/          PeerManage + RegistrySync
  connectivity/   STUN / 映射观察 / NAT 探测
  management/     AntiAbuse、JailFile、License、AuthChallenge、ConnectAuth、RelayHealth、Status
  rpc/            on_msg_*
  listener/       bind + 防回环
  proxyserver/    专用中继
  wakeserver/     IoT 唤醒
```

`instance` 不得再长出同步协议、代理择优、Token 验签实现。

---

## 4. 明确不抄 / 不做

- TUN / DHCP / 子网代理 / SOCKS / KCP / QUIC / 魔法 DNS / ACL 组密钥
- mesh / `--p2p-only` / gateway / 多配置多虚拟网 / WASI
- 在 juice 状态/recv 回调里调任何 `juice_*`（含 `juice_get_selected_addresses`）
- 持 `mu_` 的 worker tick 里调 juice 查询 API
- 把 P9–P11 塞进 `p2p_natserver`

---

## 5. 配置速查

| 键 / 环境 | 作用 |
|-----------|------|
| `PrivateMode` / `P2P_PRIVATE_MODE` | 强制 `EnableAuth`（仍需 `AuthSecret`） |
| `JailFile` / `P2P_JAIL_FILE` | fail2ban 导出路径 |
| `StatusAllow` / `P2P_STATUS_ALLOW` | 状态口 CIDR 白名单 |
| `P2P_NEED_P2P` / `peer -need-p2p` | 本端尽快直连，并宣告 `np=1` |
| `force_relay` / `peer -relay` | 硬关打洞 |

覆盖顺序：文件 < 已设置的 `P2P_*` < 命令行。`${ENV}` 可关：`P2P_DISABLE_ENV_PARSING=1`。

---

## 6. 测试

单测：`punch_admit_test`（NEED / compose）、`cfg_file_test`（JailFile）、
`server_arch_test`（注册表 `np=1`、ConnectAuth 门、jail 导出）。

端到端：`test.sh` [0]–[21]。信令核收口当轮曾 **PASS=139 FAIL=1**（[16] ICE restart 日志竞态）。
整体优化轮已在客户端用锁外 `juice_get_state` + `fflush` 收口该路径，见
[`项目整体优化分析.md`](项目整体优化分析.md)。

RegistrySync 日志必须仍含：

- `registry sync enabled, %zu peer(s)`
- `sync entry uuid[%s]`
- `sync snapshot sent`

本环境注意：无 `killall`（用 `pkill -f` + `[x]`）；不要在同一条 shell 里同时写 `peer`/`iotc_demo` 路径和 `pkill -f`。
`Killed` 来自 cleanup 的 `kill -9`，不是失败。

---

## 7. 不算未完成的项

| 项 | 原因 |
|----|------|
| 10 万设备心跳压测 | 需要压测机与现网容量规划，不是再改信令分发 |
| 真 `tc netem` / docker+iptables NAT 矩阵 | 本环境无 iptables/`ip`，`unshare` 被拒 |
| 三平台 SDK（Android/iOS/Windows） | 客户端打包，不在 p2p_server |
| 弱网 1080p / 唤醒出图 <6s | 现网设备与码流，不在信令核 |
| P9 SFU / 28181 | 独立网关进程 |
| P10 WHIP/WHEP | 独立网关进程 |
| P11 MoQ | 预研笔记，不进 1:1 主路径 |

配套：[`P2P服务器开发计划书.md`](P2P服务器开发计划书.md)、
[`EasyTier指南架构对照.md`](EasyTier指南架构对照.md)、
[`EasyTier配置对照.md`](EasyTier配置对照.md)、
[`server/README.md`](../server/README.md)。
