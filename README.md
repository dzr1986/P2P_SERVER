# p2p_server_byding

基于对原始二进制 `p2p_server`(NatServer) 与 `proxy_server`(P2PProxy) 的逆向分析，**从零实现的一套同架构 P2P NAT 穿透服务**。

- 原始分析见仓库根目录 [`分析报告.md`](../分析报告.md)
- 目标：以可编译、可运行、可替换的开源实现，还原原版"UDP 打洞 + 中继兜底"的服务端架构
- 语言：C++17（无第三方依赖，仅 libc/libstdc++）

## 功能

| 组件 | 二进制 | 对应原版 | 功能 |
|------|--------|----------|------|
| NatServer | `server/natserver/bin/p2p_natserver` | `p2p_server` | UDP 汇聚/打洞协调：UUID 注册、心跳保活、公网地址交换、CONNECT 请求转发、设备/服务器列表、防洪水与 IP/UUID 拉黑（持久化）、HMAC-SHA256 鉴权、NAT 类型探测、跨服注册表同步、运行时黑名单管理 |
| P2PProxy | `server/proxyserver/bin/p2p_proxy` | `proxy_server` | UDP 中继兜底：代理注册、RELAY_DATA 转发、可用性查询、协助打洞 |
| 测试客户端 | `client/bin/peer` | （原版设备侧 SDK） | 注册 -> 鉴权 -> CONNECT -> 打洞直连 -> 失败走中继，端到端演示 |

另外提供 **可复用的客户端 SDK**：编排层在 [`core/`](core/README.md)（按 EasyTier 目录拆：
`foundation → socket → packet → tunnel → connectivity → instance`），对外 UID API 在
`client/sdk/iotc/`。演示程序 `client/demo/peer.cpp`。导读见 [`docs/开发指南.md`](docs/开发指南.md)。
ICE/STUN/TURN 与流媒体通道对照见 [`docs/P2P穿透与流媒体实践.md`](docs/P2P穿透与流媒体实践.md)。

## 目录结构

```
p2p_server_byding/
├── Makefile
├── P2pServers.cfg            # 服务器地址配置
├── start.sh / stop.sh        # 启停（NatServer 16001 / P2PProxy 16002，与原版 8832/8833 区分）
├── test.sh                   # 本地端到端测试（端口 18832/18833，避免冲突）
├── core/                     # 按 EasyTier 层拆的共享核（见 core/README.md）
│   ├── foundation/           # 时钟/加密/UID（无网络域）
│   ├── socket/               # UDP/TCP/TLS 端点
│   ├── packet/               # ProtoDef / Codec / STUN / ICE SDP
│   ├── tunnel/               # Session 可靠帧
│   ├── connectivity/         # STUN 探测 / 打洞 / PathSelect
│   └── instance/             # P2PClient 门面
├── common/                   # 旧路径兼容头（转发到 core/）
├── server/                   # p2p_server 按 EasyTier 目录拆（见 server/README.md）
│   ├── config/               #   P2pServers.cfg
│   ├── peers/                #   UUID 注册表（不做 mesh 路由）
│   ├── connectivity/         #   服务端 STUN / NAT 探测
│   ├── management/           #   防滥用 / 白名单 /metrics
│   ├── rpc/                  #   RecvProcess 报文分发
│   ├── instance/             #   NatServer 生命周期
│   ├── proxyserver/          #   专用中继（DERP，不是 gateway）
│   └── wakeserver/           #   IoT 唤醒
└── client/
    ├── sdk/iotc/             # TUTK 形 UID API（包着 core/instance）
    └── demo/peer.cpp         # 测试对端（-s 密钥 / -relay 强制中继）
```

> 文件划分与函数命名对齐原始分析（`NatServer.cpp`、`RecvProcess.cpp`、`PeerManage.cpp`、
> `CfgFile.cpp`、`P2PProxy.cpp`），方便对照原版调试符号。

## 架构

```
           ┌────────────────────────────┐
           │   NatServer (UDP 打洞协调)  │
           └───────┬──────────┬─────────┘
        注册/心跳/地址交换  CONNECT转发
                   │          │   SP_ASK_EXTINFO(代理可用性)
         ┌─────────┴──┐   ┌───▼──────────┐
         │  设备A      │   │  P2PProxy    │
         │ 直接UDP打洞 │   │ 中继转发(兜底)│
         │  设备B      │   └──────────────┘
         └────────────┘
```

1. 同网段先组播 **LAN Search**（`239.255.77.89:17890`），命中则记下局域网地址
2. 设备/App 以 **UUID** 向 NatServer 心跳注册，NatServer 记录其观察到的公网地址
3. 需要连接时，发起方发 `CONNECT_REQ`，NatServer 回 `CONNECT_ACK` 并 `CONNECT_INVITE` 通知目标方
4. `CONNECT_OK` 后发起方 ICE gather（controlling），被邀方先 `set_remote` 再 gather；打通后 P2P
5. 打洞失败（对称 NAT 等）→ 双方注册到 **P2PProxy**，改走中继；中继后仍后台打洞，成功则回切

## 报文协议

### 统一报文头（8 字节）

```
+--------+---------+---------+-----------+
| magic  | version | msg_id  |  length   |
| (2B,BE)|  (1B)   |  (1B)   |  (4B,BE)  |
+--------+---------+---------+-----------+
magic = 0x584E ("XN")，length = payload 字节数（不含头）
```

### 消息 ID

| ID | 名称 | 方向 | 说明 |
|----|------|------|------|
| 0x01 | HEARTBEAT_REQ | 客户端->NatServer | 心跳注册（UUID+设备类型+私网端口），应答 0x02 |
| 0x02 | HEARTBEAT_RSP | NatServer->客户端 | 回传观察到的公网 IP/端口 |
| 0x03 | SND_EXTINFO_REQ | 客户端->NatServer | 上报本机地址 |
| 0x04 | ASK_EXTINFO_REQ | 客户端->NatServer | 查询公网地址，应答 0x05 |
| 0x05 | ASK_EXTINFO_RSP | NatServer->客户端 | 公网 IP/端口 |
| 0x06 | CONNECT_REQ | 发起方->NatServer | 请求连接目标 UUID |
| 0x07 | CONNECT_ACK | NatServer->发起方 | 目标公网/私网地址 + 兜底代理 |
| 0x08 | CONNECT_INVITE | NatServer->目标方 | 发起方公网/私网地址 + 兜底代理 |
| 0x09 | GET_DEV_LIST_REQ | 客户端->NatServer | 在线设备列表（startIndex/wantNum），应答 0x0A |
| 0x0A | GET_DEV_LIST_RSP | NatServer->客户端 | 设备条目列表 |
| 0x0B | GET_SERVER_LIST_REQ | 客户端->NatServer | 服务器列表查询，应答 0x0C |
| 0x0C | GET_SERVER_LIST_RSP | NatServer->客户端 | NAT 服务器 + 代理地址列表 |
| 0x0D | ADD_UID_REQ | 客户端->NatServer | 主动注册 |
| 0x0E | DELETE_UID_REQ | 客户端->NatServer | 注销 |
| 0x0F | CHECK_UID_REQ | 客户端->NatServer | 校验 UUID 是否存在，应答 0x12 |
| 0x10 | SP_ASK_EXTINFO_REQ | NatServer->Proxy | 代理可用性查询，应答 0x11 |
| 0x11 | SP_ASK_EXTINFO_RSP | Proxy->NatServer | used/max/available |
| 0x12 | CHECK_UID_RSP | NatServer->客户端 | 1 字节 result |
| 0x13 | NAT_DETECT_REQ | 客户端->NatServer | NAT 类型探测（主/备双 socket 各应答一条 0x14） |
| 0x14 | NAT_DETECT_RSP | NatServer->客户端 | 探测应答：server_index + 观察到的主/备端口 |
| 0x15 | AUTH_CHALLENGE_REQ | 客户端->NatServer | 鉴权挑战请求 |
| 0x16 | AUTH_CHALLENGE_RSP | NatServer->客户端 | 挑战应答（返回 nonce） |
| 0x17 | AUTH_LOGIN_REQ | 客户端->NatServer | 登录：uuid + nonce + HMAC-SHA256(uuid+nonce) |
| 0x18 | AUTH_LOGIN_RSP | NatServer->客户端 | result（0=OK） |
| 0x19 | ADMIN_STATS_REQ/RSP | 客户端->NatServer | 管理统计查询/应答（AdminStatsRsp） |
| 0x1D | ADMIN_BLACKLIST_REQ | 客户端->NatServer | 黑名单管理：op+key+HMAC(AdminSecret,op+key)，应答 0x1E |
| 0x1E | ADMIN_BLACKLIST_RSP | NatServer->客户端 | 结果+总数+明细（AdminBlacklistRsp） |
| 0x1F | ICE_SDP | 经 NatServer 或局域网 | ICE local description（dst/src UID + SDP）；跨服时本机无 dst 则经 sync 对端转发 |
| 0x40–0x43 | WAKE_* | wakeserver | 低功耗保活 / 触发 / POKE |
| 0x50 | LAN_QUERY | 局域网组播 | 按 UID 询问同网段设备 |
| 0x51 | LAN_ANNOUNCE | 局域网组播 | 宣告本机 UID + 主 socket 端口 |
| 0x30 | SYNC_PEER_ENTRY | NatServer<->NatServer | 注册表同步：单条对等节点增/更（SyncPeerEntry） |
| 0x31 | SYNC_PEER_DEL | NatServer<->NatServer | 注册表同步：对等节点删除（SyncPeerDel） |
| 0x32 | SYNC_SNAPSHOT_REQ | NatServer->NatServer | 全量快照请求（SyncSnapshotReq 时间戳，应答为多条 0x30） |
| 0x20 | PROXY_REGISTER_REQ | 客户端->Proxy | 代理注册（UUID），应答 0x21 |
| 0x21 | PROXY_REGISTER_RSP | Proxy->客户端 | result + 公网地址 |
| 0x22 | PROXY_UNREGISTER_REQ | 客户端->Proxy | 注销注册 |
| 0x23 | PROXY_RELAY_DATA | 客户端<->Proxy | RelayFrame(src,dst) + 业务负载，按 dst 转发 |
| 0x24 | PROXY_PUNCH_HELPER | 客户端->Proxy | 协助打洞：返回目标当前公网地址 |

### 打洞后的直连数据（无报文头）

`PeerData`：`"PDAT" + type + seq + src_uuid + msg`，type 0=ping 1=pong 2=punch。

完整结构体定义见 `common/ProtoDef.h`（均为 1 字节对齐，数值字段网络字节序）。

## 构建与运行

```sh
make                 # 编译三个二进制
bash test.sh         # 本地端到端测试（两个对端打洞直连）
./start.sh           # 以后台方式启动 NatServer(16001) + P2PProxy(16002)
./stop.sh            # 停止
```

命令行参数（对齐原版）：

```sh
./server/natserver/bin/p2p_natserver <NatServerPort> <ProxyServerPort> <WanIP> [P2pServers.cfg]
./server/proxyserver/bin/p2p_proxy <Port> <MaxProxyNum> [Workers] [QuotaMB] [AltPort] [TcpPort]
# AltPort 未给 TcpPort 时兼听 TCP/TLS（对标 443）。证书：P2P_PROXY_TLS_CERT / P2P_PROXY_TLS_KEY
# 启动脚本：P2P_PROXY_ALT_PORT=443 P2P_PROXY_TCP_PORT=443 ./start.sh public
./client/bin/peer <NatServerIP> <NatServerPort> <UUID> [对端UUID] [ProxyIP] [ProxyPort] [-s 密钥] [-relay] [-restart]
```

`-s` 携带鉴权密钥（服务端开启 EnableAuth 时必须）；`-relay` 强制走中继（用于验证兜底路径）。

### 测试客户端用法示例

```sh
# 终端1：等待方（无鉴权模式）
./client/bin/peer 1.2.3.4 16001 A
# 终端2：发起方（连接 A，强制中继）
./client/bin/peer 1.2.3.4 16001 B A 1.2.3.4 16002 -relay
# B 输出 "CONNECTED to A via direct" 表示打洞直连成功；
# 输出 "CONNECTED to A via relay" 表示经 P2PProxy 中继成功。
```

## 配置

`P2pServers.cfg`（客户端据此发现服务器，NatServer 据此构建服务器列表并探测代理）：

```
# 服务器地址（支持 ip 或 ip:port，# 开头为注释）
NatServer1=127.0.0.1       # NAT 汇聚服务端（可多台）
Proxy1_1=127.0.0.1         # 中继代理（可多个）

# 鉴权（可选，缺省关闭）
EnableAuth=1               # 1=强制鉴权（客户端必须带 -s 密钥）
AuthSecret=s3cr3t          # HMAC-SHA256 密钥（开启鉴权后，心跳走流加密通道）

# UUID 白名单（可选，缺省放行全部；与 EnableLicense 二选一即可）
EnableLicense=1            # 1=启用白名单门控
AllowedUuids=aaaa,bbbb     # 白名单 UUID（逗号分隔）
# 或直接：WhitelistUuids=aaaa,bbbb   （写即启用白名单）

# License 文件持久化（可选；启用后白名单写入 LicenseFile，重启不丢）
LicenseFile=/etc/p2p/license.dat  # 白名单持久化文件路径
LicensePass=lic-pass              # 加密口令（非空=PBKDF2+AES-256 加密落盘）

# 并发（可选）
ProcWorkers=N              # 业务处理线程数（默认 4）
RecvThreads=N              # UDP 收包线程数（SO_REUSEPORT，默认 2，最大 16）

# 多 NatServer 注册表同步（可选；多台 NAT 汇聚服务端互相同步在线表）
SyncPeers=1                # 1=启用注册表同步（跨服 CONNECT 协调）
SyncAddrs=1.2.3.4:8832,5.6.7.8:8832   # 同步对端（缺省用 NatServer* 列表；port 缺省取本机端口）
SyncAuthSecret=sync-secret # 同步消息 HMAC 密钥（非空则增量/快照全部签名，防伪造注入）

# 黑名单持久化（可选；IP/UUID 拉黑重启不丢）
BlacklistFile=/etc/p2p/blacklist.dat  # 黑名单文件路径
BlacklistPass=bl-pass                 # 文件口令（非空=PBKDF2+AES-256 加密落盘）

# 运行时管理接口（可选；AdminSecret 不配置则接口整体拒绝）
AdminSecret=admin-secret   # 黑名单管理接口校验密钥（HMAC-SHA256 防伪，不在线上传明文）

# 防洪水（可选）
FloodPktThreshold=200      # 1s 内最大包数
BlacklistSeconds=60        # 触发后拉黑时长（秒）
```

部署到公网时改为真实公网 IP；`start.sh` 中 WanIP 参数也同步替换。

## 鉴权流程

1. 客户端先发 `AUTH_CHALLENGE_REQ`，服务端回 `AUTH_CHALLENGE_RSP`（含一次性 nonce）
2. 客户端计算 `mac = HMAC-SHA256(secret, uuid || nonce)`，发 `AUTH_LOGIN_REQ`
3. 服务端校验通过后回 `AUTH_LOGIN_RSP result=0`，并将 uuid 加入鉴权会话表（默认 1h 过期）
4. 未鉴权或校验失败的 uuid：心跳返回 result=1，CONNECT 协调一律拒绝（result=3）
5. 鉴权会话按 uuid 独立保存（与注册表解耦），登录成功后客户端立即重发等待中的 CONNECT 请求
6. 白名单（`EnableLicense`/`AllowedUuids`/`WhitelistUuids`）在心跳/CONNECT/挑战/登录四处统一门控，
   不在白名单的 uuid 一律拒绝（挑战 result=2、登录 result=4），客户端不再重试

## 心跳加密

开启鉴权（`EnableAuth=1`）后，客户端登录成功即切换**加密心跳**：

- 请求 `MSG_HEARTBEAT_REQ_ENC(0x1B)`：负载 = `uuid(33B 明文，路由用) || iv(8) || cipher(UuidReq+扩展)`
- 应答 `MSG_HEARTBEAT_RSP_ENC(0x1C)`：负载 = `uuid(33B) || iv(8) || cipher(ExtInfoRsp)`，复用请求 IV
- 密钥流：`keystream_block(i) = HMAC-SHA256(AuthSecret, uuid(33B)||iv||i)`，逐块 XOR（见 `Crypto.h p2p_stream_xor`）
- 密钥派生绑定 uuid + 每包随机 IV，防重放/篡改；未开启鉴权时仍走明文心跳（兼容）

## 多收包线程

- `RecvThreads=N`：主收包线程（主 socket）+ N-1 个 `SO_REUSEPORT` 克隆 socket（同端口），
  由内核在多 socket 间均衡分发 UDP 包，解除单 epoll 收包瓶颈
- NAT 探测仍走快速路径（任意收包线程命中即双 socket 应答，不影响打洞）
- 其余报文入有界队列，由 `ProcWorkers` 处理线程池消费

## 多 NatServer 注册表同步

开启 `SyncPeers=1` 后，多台 NatServer 之间的在线对等节点表自动同步，跨服 CONNECT 可直接协调：

- **增量广播**：本机节点注册/心跳更新即向所有同步对端广播 `MSG_SYNC_PEER_ENTRY`（hop 从 1 逐跳
  +1，超过 `SYNC_HOP_MAX=3` 停止，防环）；节点离线/删除广播 `MSG_SYNC_PEER_DEL`。
- **全量快照**：启动即发 `MSG_SYNC_SNAPSHOT_REQ`，之后每 `SYNC_SNAPSHOT_INTERVAL=30s` 周期拉取；
  收到请求方以多条 `MSG_SYNC_PEER_ENTRY`（hop=`SYNC_HOP_SNAP=0xFF`，不转发）应答本机全量表。
- **去重**：`seen_sync_entry` 按 `uuid|hb_time|src` 记录 300s 内见过的条目，重复/回环包直接丢弃。
- **同步鉴权**：配置 `SyncAuthSecret` 后，所有同步消息（增量/删除/快照请求与应答）在负载末尾
  附加 `HMAC-SHA256(SyncAuthSecret, body)` 32B 标签；接收端校验不通过即丢弃并告警。防环转发时
  重新签名（hop 变化）；快照请求携带时间戳，接收端校验 300s 新鲜度防重放。不配置密钥则保持旧版
  明文兼容（任意来源可注入，生产必配）。
- **跨服 CONNECT**：同步条目保留源服务器侧心跳时间与鉴权状态（视为已通过鉴权），
  目标方地址即源服务器观察到的公网地址，跨服打洞/中继无需客户端感知。
- 同步对端默认取配置 `NatServer*` 列表（排除本机 `WanIP:Port`），也可用 `SyncAddrs=ip:port,...` 显式指定。

## 黑名单持久化与运行时管理

- **持久化**：配置 `BlacklistFile` 后，IP/UUID 黑名单（含过期时间）由定时器周期性落盘，
  重启自动加载，防滥用拉黑不因进程重启丢失。文件明文头 `#P2P-BLACKLIST-v1`，每行
  `IP <点分> <过期epoch>` / `UUID <uuid> <过期epoch>`；配置 `BlacklistPass` 则整体按
  PBKDF2-HMAC-SHA256(10000 次) + AES-256-CBC 加密落盘（魔数 `P2PB`），明文格式自动识别。
- **运行时管理接口**：`MSG_ADMIN_BLACKLIST_REQ(0x1D)/RSP(0x1E)`，op=1/2/3/4/5 分别
  加/删 IP、加/删 UUID、查询列表。请求体 = `op(1) || key(64B) || HMAC-SHA256(AdminSecret, op||key)`，
  未配置 `AdminSecret` 或 MAC 不符一律拒绝（密钥明文不上线）；应答返回总数与（最多 28 条）明细。
- **代理可用性调度**：服务端每 10s 向配置代理探测可用性/占用率，记录连续失联次数（`down`）。
  调度时对健康代理按空闲度（1 - used/max，扣失联罚分）**加权随机**选出至多 3 个候选，
  分散首选负载、天然失败转移；候选不足时按配置顺序回退补齐 3 个入口。

## 安全与扩展说明

- 本实现为**协议与架构对等**的干净重实现，无第三方依赖（C++17 + libc/libstdc++）。
- 已实现：HMAC-SHA256 挑战应答鉴权、UUID 白名单门控、心跳载荷流加密、**打洞数据面加密**、
  **License 文件 PBKDF2+AES-256 对等实现**、防洪水与 IP/UUID 拉黑、**黑名单文件持久化**、
  **运行时黑名单管理接口（AdminSecret HMAC 防伪）**、**同步消息 HMAC 鉴权**、
  **代理可用性加权调度**、多收包线程（SO_REUSEPORT）、配置热加载、多 NatServer 注册表同步。

## 测试结果

`bash test.sh` 全量断言（42 项）示例输出：

```
== [1] direct P2P (no auth) ==
  ok: A->B direct
  ok: B->A direct
  ok: A nattype detected
  ok: B nattype detected
  ok: A received B data
== [2] relay fallback (-relay) ==
  ok: A->B relay
  ok: B->A relay
  ok: proxy registration
== [3] auth + encrypted heartbeat (EnableAuth=1, RecvThreads=3) ==
  ok: multi-recv threads (3)
  ok: recv clone sockets
  ok: C auth login
  ok: D->C direct (auth)
  ok: C encrypted heartbeat accepted
  ok: E rejected without secret
== [4] UUID whitelist (EnableLicense=1 + AllowedUuids) ==
  ok: WW (whitelisted) registered
  ok: YY rejected by whitelist
  ok: WW auth OK
== [5] tunnel data-plane encryption (auth + direct) ==
  ok: G->F direct (enc)
  ok: G decrypted F data
  ok: F frames encrypted on wire
  ok: G frames encrypted on wire
  ok: G decrypted inbound frames
== [6] multi-NatServer registry sync (SyncPeers=1) ==
  ok: S1 sync peers
  ok: S2 sync peers
  ok: S2 learned A via sync
  ok: B->A direct (cross-server)
  ok: S1 snapshot response
== [7] blacklist persistence + admin mgmt (BlacklistFile/AdminSecret) ==
  ok: admin add IP
  ok: admin add UUID
  ok: admin query counts (1/1)
  ok: query lists IP
  ok: query lists UUID
  ok: bad admin mac rejected
  ok: blacklist file has IP
  ok: blacklist file has UUID
  ok: blacklist reloaded on boot
  ok: persisted counts after restart (1/1)
  ok: admin del IP
  ok: IP removed after del
== [8] cross-server sync auth (SyncAuthSecret HMAC) ==
  ok: signed sync entry accepted
  ok: B->A direct (signed sync)
  ok: forged sync entry rejected
PASS=42 FAIL=0
ALL TESTS PASSED
```

覆盖：无鉴权直连、NAT 类型探测（full-cone）、强制中继兜底、开启鉴权后直连、
加密心跳、多收包线程、无密钥被拒、UUID 白名单放行/拒绝、**打洞数据面加密**、
**多 NatServer 注册表同步与跨服直连**、**黑名单持久化与运行时增删查（含伪造 MAC 拒绝）**、
**同步消息 HMAC 鉴权（伪造条目拒绝）**。
另附独立单元测试 `tests/bin/crypto_test`（NIST SP 800-38A AES-256-CBC 向量、
加解密往返、错误密钥、PKCS7、PBKDF2 确定性）。

## 开发日志

### 阶段一：鉴权闭环（HMAC 挑战应答 + 会话表解耦）

- **死锁修复**：`auth_expire` 原存于 `peers_`（注册表），而该表本身受鉴权门控 →
  未鉴权 uuid 永远进不了表 → 心跳被拒 → 客户端无限"登录→被拒"循环。
  改为独立 `auth_sessions_` 表（uuid→过期时间），登录成功即记录、定时清理，与注册表解耦。
- **客户端竞态修复**：鉴权完成前发出的 CONNECT 会被拒绝且不重试；登录成功后
  自动重发所有等待中的连接请求。
- **验证**：4 种场景端到端通过（直连/中继 × 鉴权/免鉴权）。

### 阶段二：心跳加密 + 多收包线程 + UUID 白名单

- **心跳加密**：开启鉴权后客户端自动切换 `MSG_HEARTBEAT_REQ_ENC/RSP_ENC`，
  HMAC-SHA256 派生密钥流 + 每包随机 IV 的 XOR 流加密，uuid 明文仅用于路由。
  修复了客户端加密整段 `UuidReq`、服务端却按"仅尾部加密"解析的对齐 Bug。
- **多收包线程**：`RecvThreads=N` 用 `SO_REUSEPORT` 克隆 socket 均衡收包，
  主 socket + N-1 克隆，NAT 探测快速路径在所有收包线程均可用。
- **UUID 白名单**：`EnableLicense`/`AllowedUuids`（别名 `WhitelistUuids`）
  在心跳/CONNECT/挑战/登录四处统一门控，拒绝结果区分黑名单(1/3)与白名单(2/4)，
  客户端收到永久性拒绝（白名单/黑名单/MAC 错）后停止重试。
- **验证**：全量 17 项断言通过，零警告编译。

### 阶段三：打洞数据面加密 + License PBKDF2+AES-256 对等实现

- **打洞数据面加密**：新增 `TF_ENC=0x02` 标志位。直连帧负载 >0 即加密，
  格式 = 14B 帧头（type/seq/ack 明文）+ `iv(8)` + 密文；帧头 length 字段同步 +8。
  对端间密钥 = `HMAC-SHA256(AuthSecret, "P2P-TUNNEL-KEY:"||min(uuid)||":"||max(uuid))`，
  密钥流复用 `p2p_stream_xor`（域分隔标签 `"P2P-TUNNEL-LABEL"` 作 PRF 输入）。
  只要配置了 `AuthSecret` 即自动开启；收发均有原子计数器与访问器，peer 演示程序
  每 200ms 打印 `tunnel-enc tx=/rx=`。
- **License 文件加密持久化**：`LicenseMgr` 加密落盘，文件格式
  `magic "P2PL"(4) ver(1) iter(4BE) salt(16) iv(16) ciphertext`，
  PBKDF2-HMAC-SHA256 10000 次迭代 + AES-256-CBC（PKCS7）。`load()` 自动识别
  加密/明文旧格式；错误口令返回 false 且不覆盖文件。新增配置键
  `LicenseFile`（路径）与 `LicensePass`（口令，非空才加密）。
- **AES-256 自研实现与排错**：`Crypto.cpp` 内新增 AES-256 全套（运行时自建 S-box、
  密钥扩展、区块变换/逆变换）与 `pbkdf2_hmac_sha256`、`aes256_cbc_encrypt/decrypt`、
  `p2p_random_bytes`（/dev/urandom）。排错记录：
  1. **MixColumns 列索引 Bug**：行优先字节布局下列应为 `t[4c..4c+3]`，原误写
     `t[c],t[4+c],t[8+c],t[12+c]`；InvMixColumns 同错。修复后加密输出与
     FIPS-197 A.2 `8ea2b7ca...` 一致。
  2. **InvShiftRows 反向映射 Bug（两处）**：行 1 逆变换应为 `t[1]=st[13], t[5]=st[1],
     t[9]=st[5], t[13]=st[9]`，循环内一处与"最后一轮"一处均被误写为
     `t[5]=st[9], t[13]=st[1]`。只修循环内未修最后一轮，导致解密结果第 4/13 字节
     恰好差 0x80（借 OpenSSL 与 pycryptodome 独立 oracle 逐轮定位）。
- **验证**：`make all` 零警告；`tests/bin/crypto_test` 通过 NIST SP 800-38A
  AES-256-CBC 向量、往返、错误密钥、PKCS7、PBKDF2；`bash test.sh` 全量 22 项断言
  `PASS=22 FAIL=0`（含 [5] 数据面加密用例）。

### 阶段四：多 NatServer 注册表同步（跨服 CONNECT 协调）

- **协议**：新增 `MSG_SYNC_PEER_ENTRY(0x30)` / `MSG_SYNC_PEER_DEL(0x31)` /
  `MSG_SYNC_SNAPSHOT_REQ(0x32)`。同步条目 `SyncPeerEntry` 携带 uuid、公/私网地址、
  设备/NAT 类型、源侧心跳时间戳与扩展信息。
- **增量广播 + 全量快照**：本地节点增/更/删立即广播；启动与每 30s 请求对端全量快照。
  防环：条目 hop 逐跳 +1，达 `SYNC_HOP_MAX=3` 停止；快照条目 hop=`SYNC_HOP_SNAP` 不转发；
  `seen_sync_entry` 按 `uuid|hb_time|src` 去重（300s 窗口），重复/回环包丢弃。
- **跨服 CONNECT**：`PeerManager::upsert_synced` 保留源服务器心跳时间并把同步条目视为
  已鉴权（`auth_expire = hb_time + 3600`），目标公网地址即源服务器观测值，客户端无感跨服打洞。
  心跳超时清理改为 `cleanup_timeout_and_collect`，回收的 uuid 自动广播 `MSG_SYNC_PEER_DEL`。
- **跨服 ICE_SDP**：本地没有 `dst_uuid` 时不再 drop，而是经同步对端转发（`except=from`
  防环）。`SyncAuthSecret` 非空时转发尾部带 HMAC，对端校验后剥掉再投递给客户端。
  避免 B 刚报到对端、本服尚未 sync 到 B 时 controlling 的 SDP 丢失。
- **配置**：`SyncPeers=1` 启用；`SyncAddrs=ip:port,...` 显式指定对端（缺省用 `NatServer*`
  列表并排除本机 `WanIP:Port`）。
- **验证**：`bash test.sh` 新增 [6] 双 NatServer 用例——A 注册 S1、B 注册 S2 并跨服连接 A，
  `B->A direct (cross-server)` 通过，`PASS=27 FAIL=0`，零警告编译。

### 阶段五：黑名单持久化 + 同步鉴权 + 代理调度 + 运行时管理接口

- **黑名单持久化**（`AntiAbuse`）：`BlacklistFile`/`BlacklistPass` 配置键，`dirty_` 标记变更，
  定时器周期落盘、重启 `load()` 恢复；加密文件格式 `magic "P2PB" + PBKDF2+AES-256`（复用
  LicenseMgr 原语），明文头 `#P2P-BLACKLIST-v1` 自动识别。新增 `set_path/set_password/load/save/
  needs_save/reset_dirty` 与 `unblacklist_ip/uuid`、`ip/uuid_blacklist_snapshot`。
  排错记录：明文 IP 行 `IP <点分> <epoch>` 只有 2 个空格，原解析器误以为需要第三个空格定位
  过期时间（`sp2`）导致 IP 永远加载失败，改为 `sp1 之后到行尾` 取 epoch 修复。
- **跨服同步鉴权**：`SyncAuthSecret` 配置后所有 `MSG_SYNC_*` 负载末尾附加 32B
  `HMAC-SHA256(secret, body)`；转发重签、快照请求带时间戳防重放（±300s）。`sync_verify(payload,
  plen, body_len)` 明确"总长/被签名长"两个维度，避免把 MAC 当作正文校验（首次实现曾因维度混淆
  导致合法包全被拒——正文 4B 的 snapreq 直接 `plen<32` 返回 false，79B 的 entry 把 MAC 位置
  读在正文中间）。
- **代理可用性调度**：`ProxyHealth` 增加连续失联计数 `down`，定时探测未应答即降权并置不可用；
  `pick_proxy` 改为对健康代理按空闲度加权随机选 3 个（分散首选、天然失败转移），不足时按配置
  顺序回退补齐。
- **运行时黑名单管理**：`MSG_ADMIN_BLACKLIST_REQ/RSP(0x1D/0x1E)`，op=1/2/3/4/5 加/删 IP、
  加/删 UUID、查询（最多 28 条明细，控制 `<= MAX_PKT`）。请求鉴权用
  `HMAC-SHA256(AdminSecret, op||key)`，未配置或 MAC 不符一律拒绝，密钥明文不出网。
- **验证**：`bash test.sh` 新增 [7]（持久化 + 管理增删查 + 伪造 MAC 拒绝 + 重启恢复）与
  [8]（带 `SyncAuthSecret` 的跨服同步 + 伪造同步条目拒绝）两组用例，`PASS=42 FAIL=0`，
  零警告编译。
