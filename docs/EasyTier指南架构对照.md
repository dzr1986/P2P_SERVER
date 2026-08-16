# EasyTier `guide` → p2p_server 开发架构

> 对照源：`/home/ubuntu/easytier.github.io.git/guide/`（**不要** `git add`）。
> EasyTier 是去中心 SD-WAN。本仓库是 **有中心信令的 IoT UID**。

`guide` 按「产品怎么用」拆页。`server/` 按同样边界拆目录，方便一层一层学，而不是把 NatServer.cpp 再堆成上帝对象。

---

## 1. 从 guide 学到的架构规则

| guide 页 | 他们在说什么 | 本仓库怎么落 | 明确不抄 |
|----------|--------------|--------------|----------|
| `introduction.md` | 去中心、虚拟网、智能路由 | 只学「直连优先、中继兜底」 | 节点平等转发、TUN |
| `aboutp2p.md` | NAT 类型决定打洞难度；失败走 relay | `PunchAdmit` + `NatMatrix` | 把 7 型 NAT 做成产品开关 |
| `network/host-public-server.md` | 共享节点可「只帮建连、不转发」；`--private-mode`；集群 | NatServer=信令，P2PProxy=专用中继；`RegistrySync`；`PrivateMode` | `--relay-network-whitelist` 给别人转业务 |
| `network/p2p-optimize.md` | lazy / need / disable-p2p | `PunchPolicy`；`force_relay` **更硬** | `--p2p-only`（IoT 必须能中继） |
| `network/config-file.md` `configurations.md` | 文件 < 环境 < 命令行；RPC 白名单 | `CfgFile` + `StatusAllow` | 多配置文件多虚拟网 |
| `network/secure-mode.md` | 分级凭据、中继不可解密 | 每 UID AuthKey + ConnectToken + `AuthChallenge` | Noise / 锁定 WG 公钥 |
| `network/decentralized-networking.md` | 多 scheme 监听、不拨自己 | `LocalListeners` 防 hairpin | tcp/ws/wg/quic 监听族 |
| `network/point-to-networking.md` | 子网代理 / 手工路由 | — | **整页不抄** |
| `config/acl.md` | 组密钥 ACL | 现有 UUID 白名单即可 | 组声明 / 零信任链 |
| `network/socks5.md` `kcp-proxy.md` `magic-dns.md` | 网关能力 | — | **不进 NatServer** |
| `perf.md` | 信令与数据面分开测 | 单测模块 + `test.sh` 端到端 | 抄他们的 iperf/TUN 数字 |

---

## 2. 进程与目录（对标「共享节点 vs 中继」）

```
p2p_natserver     信令：报到 / CONNECT / STUN / 同步 / 状态口
p2p_proxy         数据面中继（DERP），不是 gateway
p2p_wakeserver    IoT 唤醒（guide 没有对等物）
```

```
server/
  instance/       生命周期组合根（只焊，不堆业务）
  config/         文件 < P2P_* < 命令行
  peers/          PeerManage 注册表 + RegistrySync 集群同步
  connectivity/   STUN responder / 映射观察 / NAT 探测
  management/     防滥用、白名单、AuthChallenge、RelayHealth、Status
  rpc/            报文分发
  listener/       bind + 防回环
  proxyserver/    专用中继进程
  wakeserver/     唤醒进程
```

`instance` 不得再长出同步协议或代理择优实现；那些分别在 `peers/RegistrySync` 与 `management/RelayHealth`。

---

## 3. 配置键（guide 用语）

| guide | 本仓库 |
|-------|--------|
| `--private-mode` | `PrivateMode` / `P2P_PRIVATE_MODE`（强制 `EnableAuth`，仍需 `AuthSecret`） |
| `--instance-name` | `InstanceName` |
| `--rpc-portal-whitelist` | `StatusAllow` |
| `--lazy-p2p` / `--need-p2p` | 客户端 `lazy_p2p` / `need_p2p` |
| `--disable-p2p` | `force_relay`（硬关，不因对端 need_p2p 打开） |
| 空 `--relay-network-whitelist` + 只转发 RPC | **默认**：NatServer 从不转发业务载荷 |
