# EasyTier 配置对照（只学编排，不抄成 SD-WAN）

> 对照源：本机只读裸仓 `/home/ubuntu/easytier.github.io.git`
> （`guide/network/config-file.md`、`configurations.md`、`p2p-optimize.md`）。
> **不要** `git add` 进本仓库。

EasyTier 是 SD-WAN / 虚拟网卡。本仓库是有中心信令的 IoT UID。
只学「配置怎么叠、哪些开关管打洞」，不学 TUN / DHCP / 子网代理 / SOCKS / KCP / QUIC / 魔法 DNS / ACL 组密钥 / mesh。

---

## 1. 覆盖顺序

EasyTier：`配置文件 < 环境变量 < 命令行`。命令行覆盖文件；`--disable-env-parsing` 只关文件里的 `${ENV}` 展开。

本仓库对齐：

| 层 | 落点 |
|----|------|
| 文件 | `P2pServers.cfg`（`server/config/CfgFile.*`） |
| 环境 | 已设置的 `P2P_*` 才覆盖（避免 CI 残留误开鉴权） |
| 命令行 | `p2p_natserver` 端口参数；`peer -lazy` / `-need-p2p` / `-relay` |

文件值支持 `${NAME}`。`P2P_DISABLE_ENV_PARSING=1` 只关展开，不关 `P2P_*` 覆盖。

---

## 2. 可学的键

| EasyTier | 本仓库 | 说明 |
|----------|--------|------|
| `--instance-name` / `--hostname` | `InstanceName` / `P2P_INSTANCE_NAME` | 状态 JSON `instance`、metrics 标签 |
| `--rpc-portal` | `StatusPort` / `P2P_STATUS_PORT` | HTTP `/` 与 `/metrics`；0=关 |
| `--rpc-portal-whitelist` | `StatusAllow` / `P2P_STATUS_ALLOW` | IPv4 或 CIDR；空=不限制 |
| `--lazy-p2p` | `lazy_p2p` / `P2P_LAZY_P2P` / `peer -lazy` | 中继已通后不后台打洞，直到有业务 |
| `--need-p2p` | `need_p2p` / `P2P_NEED_P2P` / `peer -need-p2p` | 本端尽快直连（lazy 下也立刻后台打洞） |
| `--disable-p2p` | **`force_relay`（更硬）** | 本仓库硬关打洞；对端 `need_p2p` 也不能打。测试 [2]/[11]/[21] 依赖此语义，**不要改软** |
| `--private-mode` | `PrivateMode` / `P2P_PRIVATE_MODE` | 强制 `EnableAuth`；仍需 `AuthSecret` |

服务端其它 `P2P_*` 与文件键同名（如 `P2P_AUTH_SECRET` → `AuthSecret`），见 `CfgFile.h`。

---

## 3. 明确不抄

- TUN / DHCP / `--ipv4` / `--dev-name` / `--mtu`
- `--peers` mesh、`--proxy-networks` 子网导出、`--vpn-portal`
- SOCKS / KCP / QUIC / 魔法 DNS / ACL 组密钥
- `--p2p-only`（丢弃中继流量；IoT 必须能降级中继）
- 多配置文件启动多个虚拟网

`need_p2p` 已做跨对端宣告：心跳 extinfo 带 ASCII `np=1`，注册表记 `Peer.need_p2p`，
CONNECT 任一侧需要且准入不是 Relay 时 ACK/INVITE 尾部 hint=`PUNCH_HINT_NEED=3`。
对端收到 NEED 置 `want_direct`。**`force_relay` 仍硬关打洞**，NEED 不能打开它。

其它运维键：`JailFile` / `P2P_JAIL_FILE` 导出当前拉黑 IP（一行一个，给 fail2ban）。
