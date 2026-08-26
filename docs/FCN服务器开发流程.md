# FCN 公共服务器开发流程

> 本文档**完全基于公开互联网资料**编写（FCN 官方发布信息、RFC 标准、同类开源项目文档与工程博客），
> 不依赖本仓库任何本地代码，用于指导从零实现一套 FCN 风格的公共服务器（汇聚 + 打洞协调 + 中继）。
> 所有引用来源见文末参考链接。

## 1. 背景：FCN 是什么

FCN（free connect，作者 boywhp）是一款"傻瓜式一键接入私有网络"的工具：在**免公网 IP** 环境下，
让任意联网机器透明接入服务端所在的局域网网段（类似"反向 VPN"，作者原话："frp 说白了就是一个
TCP 隧道，FCN 类似反向 VPN，直接接入目标局域网"）。

### 1.1 三方架构

```
用户服务端 (fcn_server)  <-----  FCN 公共服务器  ----->  用户客户端 (fcn_client)
  部署在目标局域网内，          （默认 s1.xfconnect.com）    任意联网机器，
  TUN 网卡 + NAT/DHCP，         信令汇聚 / 打洞协调 /        TUN 虚拟网卡接入
  向公共服务器注册在线           随机数分发 / 数据中继
                └────────────── P2P 打洞成功后直连（不限速）──────────────┘
```

- **用户服务端 / 用户客户端是开源发行的**（Windows/Linux/OpenWrt/ARM/Android 多平台二进制）；
- **FCN 公共服务器闭源**，作者在 V2EX 明确表示"暂时没有开源计划"，用户只能使用官方
  `s1.xfconnect.com`。这正是自研 FCN 服务器的动机：**实现一套协议兼容或架构对等的公共服务器，
  支持私有化部署**。

### 1.2 官方公开的关键事实（需求输入）

以下事实来自 FCN 官方 README / 论坛发布贴，是服务器实现必须满足的外部行为约束：

| 维度 | 公开事实 |
|---|---|
| 账号体系 | 测试账号 `FCN_0000`~`FCN_9999`（8 字符 UID）；付费账号 = UID + 8 位识别码（uic）；服务端配置 `psk`（管理员密码 hash 或明文）；`authfile` 用户列表文件 |
| 命名 | 每个用户服务端注册唯一"服务器名"（`name`/`--svr`），同一 UID 下以服务器名区分多个服务端实例 |
| 端口 | 服务端默认 UDP `5000`（uport，可自定义 1000-2000）、TCP `8000`（tport）；`pport` 为 P2P 通信端口（服务端可做端口映射时才填） |
| 链路 | 默认 UDP，可强制 TCP（`--tcp`）；P2P 直连优先，失败走公共服务器中继 |
| 限速/配额 | 测试账号中继限速 100KB/s、每日流量配额 150M；**P2P 直连成功后不限速**（→ 限速/计费只发生在中继路径上） |
| 安全机制 | ① 公共服务器与客户端之间 TLS 证书**双向验证**；② 用户服务端每 30 分钟向公共服务器请求随机数；③ 公共服务器用**真随机数发生器**产生随机数并经 TLS 下发；④ 客户端/服务端用 `随机码 + UID + PSK` 计算会话 key；⑤ 通信数据包全程 AES-256 加密（可选 `aes-256-cfb`/`aes-128-cfb`/`chacha20`），约 30 分钟自动更新会话密钥 |
| 隐私承诺 | 公共服务器不收集用户网络数据；支持"强制点对点通信"（数据完全不过服务器） |
| 数据面封装 | 默认只封装三层 IP 报文（TUN + NAT 模式）；Windows 服务端额外支持桥接（TAP）模式 |
| 多服务器 | 官方公共服务器以 `s1.` 编号命名，暗示多实例横向扩展 |

由此可推导出 FCN 公共服务器的**六大职责**：

1. **目录服务**：维护 `UID + 服务器名 → 在线服务端(公网地址、能力)` 注册表，处理注册/心跳/下线；
2. **地址观察（STUN 职能）**：告知每个接入方它被服务器观察到的公网 `IP:port`；
3. **打洞协调（rendezvous）**：客户端请求接入某服务端时，向双方互发对端公网地址并协调同时打洞；
4. **会话随机数分发**：作为可信第三方，周期性（30 分钟）经 TLS 下发真随机数，供两端派生会话密钥
   （服务器**不接触会话密钥本身**，只发随机数——密钥 = f(随机数, UID, PSK)，PSK 不出端）；
5. **数据中继（兜底）**：打洞失败时转发两端加密数据，并执行限速/配额/计费；
6. **账号与滥用管理**：UID/识别码校验、测试账号限额、防洪水、黑名单。

## 2. 必读协议标准（互联网知识底座）

实现打洞协调与 NAT 探测前，必须吃透以下标准（全部可公开获取）：

| 标准 | 内容 | 对本项目的意义 |
|---|---|---|
| [RFC 4787](https://datatracker.ietf.org/doc/html/rfc4787) | UDP NAT 行为需求：映射行为（EIM/ADM/APDM）与过滤行为（EIF/ADF/APDF） | 现代 NAT 分类的权威口径，NAT 探测模块的判定依据 |
| [RFC 3489](https://datatracker.ietf.org/doc/html/rfc3489) | 经典 STUN：Full Cone / Restricted / Port-Restricted / Symmetric 四分法 | 传统 NAT1-NAT4 叫法的来源（见 2.1 对照表） |
| [RFC 5389](https://datatracker.ietf.org/doc/html/rfc5389) / [RFC 8489](https://datatracker.ietf.org/doc/html/rfc8489) | 现代 STUN：Binding 请求/响应、XOR-MAPPED-ADDRESS、消息认证 | 地址观察服务可直接兼容 STUN 线协议，复用现成客户端/测试工具 |
| [RFC 5128](https://datatracker.ietf.org/doc/html/rfc5128) | P2P 跨 NAT 通信现状：UDP/TCP 打洞、hairpin、中继 | 打洞协调流程（同时打洞、保活）的教科书 |
| [RFC 8445](https://datatracker.ietf.org/doc/html/rfc8445) | ICE：候选地址收集、连通性检查、路径选择 | 多路径（内网直连/公网打洞/中继）候选与探测的框架参考 |
| [RFC 8656](https://datatracker.ietf.org/doc/html/rfc8656) | TURN：中继分配、权限、通道机制 | 中继服务的协议级参考（配额、生命周期、通道号压缩头） |
| [RFC 6887](https://datatracker.ietf.org/doc/html/rfc6887) | PCP（及前身 NAT-PMP、UPnP IGD） | 客户端侧可选的端口映射捷径（FCN 的 `pport` 即人工端口映射） |

### 2.1 NAT 类型对照表（RFC 3489 四分法 ↔ RFC 4787 行为学）

| RFC 3489 | RFC 4787 映射行为 | RFC 4787 过滤行为 | 俗称 | 打洞难度 |
|---|---|---|---|---|
| Open Internet | 端点无关映射（EIM） | 端点无关过滤（EIF） | NAT0/公网 | 无需打洞 |
| **Full Cone** | EIM | EIF | NAT1 全锥 | 极易（任何外部主机可直接回包） |
| Restricted Cone | EIM | 地址相关过滤（ADF） | NAT2 IP 受限锥 | 易（双方互发一次即通） |
| Port Restricted Cone | EIM | 地址+端口相关过滤（APDF） | NAT3 端口受限锥 | 易（需精确对端 IP:port） |
| **Symmetric** | 地址+端口相关映射（APDM） | APDF | NAT4 对称 | 难（每个目的地址映射不同外部端口，STUN 观察到的端口对 P2P 无效） |

工程结论（来自 Tailscale《How NAT traversal works》与 APNIC 转载）：

- 锥形（EIM）×锥形：标准同时打洞即可，成功率高；
- 锥形 × 对称：可用**生日悖论端口预测**——对称侧开约 256 个 socket，锥形侧以 100 包/秒随机探测,
  50% 概率 2 秒内打通，20 秒基本必通（仅探测了全空间的 <4%）；
- 对称 × 对称：生日悖论也要 ~17 万包才有 99.9% 成功率（100pps 下约 28 分钟），**工程上直接走中继**；
- 国内 ISP 现状（nat1_traversal 项目总结）：电信常见 Full Cone，移动常见 Symmetric——中继兜底必不可少。

## 3. 同类开源实现架构对比（技术选型参考）

自研前应通读这四个开源系统的服务器端设计，FCN 公共服务器的每个职责都能找到成熟对应物：

| 系统 | 控制面 | 数据面/中继 | 服务器是否接触密钥 | 借鉴点 |
|---|---|---|---|---|
| [n2n](https://github.com/ntop/n2n)（supernode） | UDP 注册/心跳（默认端口 7654，管理端口 5646） | supernode 直接中继以太帧；令牌桶按 community 限速 | 否（不持有 community 密钥，无法窥探/注入） | 单线程 `select()` 事件循环即可支撑目录+中继；注册 150s 超时清理；中继包内注入发送方公网地址触发对端反向打洞（peer pushing）；**重启丢注册表、靠客户端 5 分钟内重注册恢复**——服务器可以做成无状态 |
| [ZeroTier](https://docs.zerotier.com/protocol/) | VL1 根服务器（planet/moon）做 rendezvous；VL2 controller 做成员授权/配置/地址分配 | 根服务器转发首包并向双方发 RENDEZVOUS 提示直连 | 否 | **控制面（授权/配置）与转发面（rendezvous/中继）分层解耦**；controller 可独立自建 |
| [Tailscale](https://tailscale.com/blog/how-nat-traversal-works) / [headscale](https://github.com/juanfont/headscale) | 协调服务器（headscale 为开源自建版）管理节点/密钥/ACL | [DERP](https://tailscale.com/docs/reference/derp-servers) 中继：跑在 HTTPS(443) 上按**目标公钥**盲转发已加密包；同时兼任 STUN 服务器 | 否（私钥不出设备，DERP 无法解密） | 中继协议走 443/HTTP 可穿透严格出站防火墙；先经中继建立信令再升级直连（DISCO 探测）；`netcheck` 式的 NAT 自检产品化 |
| [frp](https://github.com/fatedier/frp) | TCP 控制连接 + token/OIDC 鉴权 | 服务器端口转发为主（xtcp 支持打洞） | 是（除非上层再加密） | 纯中继模式的配置/运营简单性；与 FCN 的差异正是"隧道 vs 虚拟组网" |
| [NetBird](https://github.com/netbirdio/netbird) | 管理服务 + 信令服务（gRPC） | 标准 ICE/STUN/TURN（coturn）+ WireGuard | 否 | 直接复用标准 STUN/TURN 生态而非自研线协议的路线 |

**选型启示**：

1. 服务器**永远不应接触数据面明文或会话密钥**——FCN 的"随机数分发 + 两端本地派生"与
   DERP 的"按公钥盲转发"殊途同归，自研时坚持这条红线；
2. 目录+协调是轻状态服务（n2n 甚至允许重启丢表），**中继才是资源大户**，两者应可独立扩缩容；
3. 中继同时提供 STUN 职能（Tailscale 模式）可省一组服务器并保证"观察点=中继点"的地址一致性；
4. TCP/HTTPS 兜底通道（FCN 的 tport=8000、DERP 的 443）是穿透企业网防火墙的关键。

## 4. 总体架构设计

```
                        ┌────────────────────────────────────────────┐
                        │              FCN 公共服务器                  │
                        │                                            │
   用户服务端 ══TLS══▶  │  ┌──────────┐   ┌──────────┐   ┌─────────┐ │
   (注册/心跳/随机数)    │  │ 接入网关   │──▶│ 在线注册表 │◀──│ 账号服务  │ │
                        │  │ (TLS 终结) │   │ uid+svr→ │   │ uid/uic/ │ │
   用户客户端 ══TLS══▶  │  │ 控制面协议  │   │ addr/cap │   │ psk hash │ │
   (查询/连接请求)       │  └────┬─────┘   └────┬─────┘   └─────────┘ │
                        │       │              │                     │
                        │  ┌────▼─────┐   ┌────▼─────┐   ┌─────────┐ │
   UDP 探测包 ────────▶ │  │ NAT 探测  │   │ 打洞协调器 │   │ 随机数    │ │
   (地址观察)            │  │ (STUN 兼容│   │rendezvous│   │ 服务(CSPRNG│ │
                        │  │  双地址)   │   └────┬─────┘   │ /dev/urandom│
                        │  └──────────┘        │         └─────────┘ │
                        │                 ┌────▼─────┐               │
   中继数据 ◀═UDP/TCP═▶ │                 │ 中继集群   │◀── 限速/配额   │
                        │                 │ (转发+计量)│               │
                        │                 └──────────┘               │
                        │  管理面：状态接口 / 黑名单 / 指标导出           │
                        └────────────────────────────────────────────┘
```

### 4.1 三个平面

| 平面 | 传输 | 内容 | 特性要求 |
|---|---|---|---|
| 控制面 | TCP + TLS（双向证书） | 注册、心跳、连接请求、地址交换、随机数下发、账号校验 | 强认证、低频、可靠 |
| 数据面（探测） | UDP | STUN 式地址观察、NAT 类型探测、打洞探测包 | 快速路径、无状态应答 |
| 数据面（中继） | UDP 为主 + TCP/443 兜底 | 两端已加密的隧道包盲转发 | 高吞吐、限速计量、零解密 |

### 4.2 核心数据模型

```
Account   { uid, uic(付费识别码), plan(test/vip), quota_daily, rate_limit, status }
ServerReg { uid, svr_name, pub_addr, lan_addr, nat_type, caps(udp/tcp/pport), last_seen, sess_nonce }
Session   { client_id, target(uid+svr_name), state(punching/direct/relayed), relay_bytes, created }
RelayBind { session, addrA, addrB, token_bucket, bytes_today }
```

注册表用内存结构即可（参考 n2n：重启后客户端心跳自动重建）；账号/配额需持久化（SQLite/嵌入式 KV
起步，量大再换）。

## 5. 开发流程（阶段化）

每阶段产出可独立验证的里程碑，按序推进；阶段 2-5 是主干路径。

### 阶段 0：需求冻结与指标定义

- 明确目标：协议兼容原版 FCN 客户端，还是仅架构对等、自带新客户端？（前者需抓包逆向线协议，
  后者自由度高——**建议后者**，避免闭源协议逆向的法律与维护风险）
- 关键指标（参考同类系统公开数据）：
  - 打洞成功率 ≥ 90%（锥形 NAT 场景接近 100%，对称×对称走中继）；
  - 打洞耗时 P50 < 2s、P99 < 20s（生日悖论场景上限）；
  - 单中继实例吞吐目标（如 1Gbps）与并发会话数；
  - 心跳周期 < 30s（**UDP NAT/防火墙映射普遍 30s 超时**，Tailscale 实测口径）。

### 阶段 1：协议设计

- 控制面消息集（TLS 内跑，序列化选定长二进制或 protobuf）：
  `REGISTER / REGISTER_ACK / HEARTBEAT / NONCE_PUSH / CONNECT_REQ / CONNECT_ACK /
   CONNECT_INVITE / RELAY_GRANT / QUOTA_REPORT / ERROR`；
- 数据面探测消息：直接**兼容 STUN Binding**（RFC 8489 magic cookie `0x2112A442` +
  XOR-MAPPED-ADDRESS），可白嫖 `stunclient`、`go-stun` 等现成工具做验证；
- 中继帧：`会话短 ID(2-4B) + 长度 + 密文`——参考 TURN Channel 机制用短 ID 替代每包携带完整
  源/目的标识，省带宽；
- 所有消息带版本号字段，预留协议协商（升级不断线）。

### 阶段 2：控制面信令服务

1. TLS 服务端：自建私有 CA，签发服务器证书 + 客户端证书（双向验证，对应 FCN 安全机制第①条）；
   证书轮换与吊销列表（CRL）从第一天设计进去；
2. 注册/心跳：`REGISTER(uid, uic?, svr_name, psk_proof, caps)` → 校验账号 → 写注册表；
   心跳刷新 `last_seen`，超时（如 90s = 3 个心跳周期）清理并通知相关会话；
   - psk 校验用**挑战应答**（服务器发 nonce，端回 HMAC(psk_hash, nonce)），明文 psk 永不上线；
3. 连接协调：客户端 `CONNECT_REQ(uid, svr_name)` → 查注册表 → 向双方分别下发对端公网+内网地址
   （`CONNECT_ACK`/`CONNECT_INVITE`），并约定同时打洞时刻；
4. 单实例事件模型：n2n 证明了单线程 event loop 足够支撑目录服务；用 epoll/kqueue +
   非阻塞 socket 起步，别过早上多线程。

**里程碑**：两个测试客户端能经 TLS 注册上线、互查在线状态。

### 阶段 3：NAT 探测与地址观察

1. STUN 兼容 Binding 应答：回 XOR-MAPPED-ADDRESS 告知观察到的公网 `IP:port`；
2. NAT 行为判定需要**两个观察点**（RFC 5780 思路）：
   - 同机双端口（主/备）判过滤行为：备端口能否直接打进客户端映射 → EIF（全锥）；
   - 双公网 IP（或双实例）判映射行为：向不同目的地址观察到的映射端口是否一致 → EIM vs 对称；
3. 探测应答必须走**快速路径**（收包线程直接回包，不入业务队列），打洞时序对延迟敏感；
4. 客户端必须**用业务 socket 本身发探测**（换 socket 映射就变了）。

**里程碑**：客户端能正确自报 NAT0-NAT4 类型，与 `go-stun`/`natter` 等第三方工具结论一致。

### 阶段 4：打洞协调

1. 标准流程（RFC 5128）：双方从协调器拿到对端 `公网 IP:port + 内网 IP:port` 后**同时**互发
   UDP 探测包；先到的包在各自 NAT 上开洞；任一方向收到包即打通；
2. 候选优先级（简化版 ICE）：同局域网直连 > 公网打洞 > 中继；对每个候选并发探测，
   全部候选保持双向 ping/pong（防止 NAT 表老化 + 防路径不对称）；
3. 对称 NAT 增强（可选二期）：生日悖论打洞——对称侧开 256 socket、锥形侧 100pps 随机探测，
   50% 两秒内打通；对称×对称直接放弃转中继（17 万包不值得）；
4. 打洞窗口超时（FCN 同类实现常用 5-10s）后自动降级中继，**对用户透明**；
5. hairpin 场景（两端在同一 NAT 后）：优先内网候选，因多数家用路由器不支持 hairpin 回环。

**里程碑**：在 netns 仿真的 NAT1×NAT1、NAT1×NAT3、NAT3×NAT3 拓扑下直连成功；NAT4×NAT4 正确降级。

### 阶段 5：中继服务

1. 会话制中继：打洞失败后双方向中继注册（凭协调器签发的 `RELAY_GRANT` 短期令牌，防伪造占坑），
   中继按会话短 ID 双向盲转发；
2. 限速与配额（对应 FCN"测试账号 100KB/s、150M/天"）：
   - 令牌桶按账号限速（n2n supernode 即内置 token-bucket 按 community 限速）；
   - 每日流量计数持久化，超配额回错误码引导升级/等待；
   - P2P 直连不经中继，天然不限速——与 FCN 行为一致；
3. TCP/HTTPS 兜底：UDP 出站被禁的网络（企业/校园）走 TCP 8000 或 443 端口的中继通道
   （DERP 全跑 HTTPS 的理由）；
4. 中继**只见密文**：帧内负载是两端用会话密钥加密后的数据，中继不持有任何密钥；
5. 独立扩缩容：中继与信令分进程/分机器部署，协调器按负载/地域给会话分配中继节点，
   并周期探测中继健康度（可用性、占用率）用于调度。

**里程碑**：强制中继模式下两端互通，限速与日配额生效，UDP 被禁环境走 TCP 兜底成功。

### 阶段 6：密钥分发与数据加密（对齐 FCN 安全机制）

按 FCN 公开的五条安全机制逐条落地：

1. TLS 双向证书验证（阶段 2 已建）；
2. 随机数服务：用 CSPRNG（`/dev/urandom` / `getrandom()`；FCN 宣称"真随机数发生器"，
   有硬件 RNG 更好）生成 32B 随机数，经 TLS 推给在线服务端；
3. 30 分钟轮换：`NONCE_PUSH` 周期下发；两端收到新随机数后**平滑换钥**（新旧密钥并行一个
   宽限期，避免换钥瞬间丢包）；
4. 会话密钥派生：两端本地计算 `key = KDF(nonce, uid, psk)`——建议用 HKDF-SHA256 并加入
   域分隔标签与双方标识，**服务器不知道 psk 故推不出 key**；
5. 数据面加密：优先选 AEAD（AES-256-GCM / ChaCha20-Poly1305）而非 FCN 原版的 CFB 流模式
   （CFB 无完整性保护，是原版可改进点）；每包随机 IV/nonce，防重放窗口。

**里程碑**：抓包验证数据面全密文；中继节点上无法还原明文；换钥期间业务不中断。

### 阶段 7：账号与运营体系

- UID 管理工具（对应官方 `authfile`/付费账号发放）：批量生成 UID+识别码、启停、改套餐；
- 防滥用：单 IP 包速阈值（防洪水）、注册频率限制、IP/UID 黑名单（带过期时间、持久化）；
- 测试账号池：`FCN_0000-9999` 式共享测试账号 + 低配额，转化漏斗的产品设计可直接沿用。

### 阶段 8：可观测性与管理接口

- 指标（Prometheus 导出）：在线服务端数、连接请求 QPS、打洞成功率/耗时分布、
  直连:中继比例、中继吞吐与配额消耗、各 NAT 类型占比；
- 管理接口：在线列表、会话查询、黑名单增删、强制下线（n2n 的 UDP 管理端口 5646 与
  headscale 的 CLI/REST 都是参考样板）；
- 结构化日志 + 采样抓包开关（只抓控制面，数据面永不落盘——隐私承诺）。

### 阶段 9：测试策略

1. 单元测试：协议编解码、KDF 向量、令牌桶边界；
2. 协议模糊测试：对控制面/探测面报文做 fuzzing（畸形长度、非法版本、重放）；
3. **NAT 仿真端到端**（Linux 单机即可）：
   - `network namespace` + veth 搭多层拓扑；
   - `iptables -j MASQUERADE`（对称行为）与 `netfilter-full-cone-nat` / `einat-ebpf`
     模块（EIM/EIF 全锥行为）模拟不同 NAT 类型；
   - `tc netem` 注入丢包/延迟/乱序，验证打洞窗口与重传；
4. 互操作验证：地址观察模块用第三方 STUN 客户端（`stunclient`、`go-stun`）回归；
5. 长稳与压测：万级心跳并发、千级并发打洞、中继打满带宽 24h。

### 阶段 10：部署与运维

- 单机起步：`systemd` 三单元（signal / stun / relay），仅开放信令 TCP 端口、探测/中继
  UDP 端口与 443 兜底；
- 多区域扩展：`s1/s2/...` 命名多实例（沿用 FCN 官方模式），客户端就近接入 +
  实例间同步在线注册表（或统一到中心目录）；中继与用户同区域部署，避免跨区双倍流量费；
- 高可用要点：信令无状态化（注册表可丢，客户端心跳重建——n2n 重启 5 分钟自愈的模式）；
  账号库主从；中继无状态、坏了换节点重挂会话；
- 证书运维：CA 私钥离线保管，服务器证书自动轮换（内部 ACME），客户端证书随安装包/账号下发。

## 6. 关键工程要点与陷阱清单

| # | 要点 | 依据 |
|---|---|---|
| 1 | 心跳周期必须 < 30s，否则 UDP NAT 映射老化断连 | 常见 UDP 状态超时 30s（Tailscale 工程实测口径） |
| 2 | 打洞探测与业务必须共用同一 UDP socket | 换 socket 即换映射，STUN 结果作废 |
| 3 | 对称×对称不要头铁打洞，直接中继 | 生日悖论下仍需 ~17 万包/99.9%，100pps 要 28 分钟 |
| 4 | 中继协议留 TCP/443 形态 | 企业网普遍禁 UDP 出站，DERP 全跑 HTTPS 即为此 |
| 5 | 服务器零密钥、零明文 | FCN 隐私承诺 / DERP"按公钥盲转发"共同红线 |
| 6 | 限速计费只作用于中继路径 | FCN 测试账号"P2P 成功后无限制"的既有产品语义 |
| 7 | 探测应答走收包快速路径，不排队 | 打洞时序敏感，排队抖动直接拉低成功率 |
| 8 | 双向保活所有活跃路径，警惕路径不对称 | ICE 特意保证双向流量，防单侧 NAT 表老化 |
| 9 | 同 NAT 下两端优先内网直连（hairpin 不可靠） | RFC 5128：多数消费级路由不支持 hairpin |
| 10 | 数据面加密用 AEAD，不用裸 CFB/XOR 流 | CFB 无完整性校验，可被比特翻转攻击（原版 FCN 可改进点） |
| 11 | 注册表可丢、账号库不可丢 | n2n supernode 重启丢表自愈模式，简化 HA |
| 12 | IPv6 双栈从第一天支持 | DERP 双栈可打通 v4-only 与 v6-only 端；v6 直连可完全绕开 NAT |

## 7. 验收清单

- [ ] 两端 TLS 双向认证注册上线，心跳保活与超时下线正确
- [ ] NAT 类型自检结论与第三方 STUN 工具一致（NAT0-NAT4 全覆盖）
- [ ] NAT1/2/3 组合场景打洞直连，P50 < 2s
- [ ] NAT4 参与场景自动降级中继，用户无感
- [ ] 中继限速 100KB/s、日配额 150M 生效；直连路径不限速
- [ ] 30 分钟随机数轮换换钥不断流；抓包全密文；中继节点无法解密
- [ ] 禁 UDP 网络下 TCP/443 兜底可用
- [ ] 防洪水拉黑、黑名单持久化、UID 管理工具可用
- [ ] netns NAT 仿真全拓扑回归通过；万级心跳压测达标
- [ ] 多实例部署 + 注册表同步 + 就近接入验证

## 8. 参考链接

**FCN 官方与社区资料**
- FCN 项目镜像（含完整 README/配置说明）：<https://gitee.com/qihangw/fcn> 、<https://github.com/al0rid4l/fcn>
- 作者 V2EX 发布贴（架构定位、"暂无开源计划"）：<https://www.v2ex.com/t/430202>
- 恩山论坛转载（安全机制五条、配置键值表）：<https://www.right.com.cn/forum/thread-318636-1-1.html>

**协议标准**
- RFC 4787（NAT UDP 行为）、RFC 5128（P2P 跨 NAT）、RFC 5389/8489（STUN）、
  RFC 8445（ICE）、RFC 8656（TURN）、RFC 6887（PCP）：<https://datatracker.ietf.org/>

**工程实践**
- Tailscale《How NAT traversal works》（生日悖论打洞、DERP 设计、30s 保活）：
  <https://tailscale.com/blog/how-nat-traversal-works>
- Tailscale DERP 服务器文档：<https://tailscale.com/docs/reference/derp-servers>
- n2n supernode 架构（事件循环、限速、重启自愈）：<https://github.com/ntop/n2n>
- ZeroTier 协议（VL1/VL2 分层）：<https://docs.zerotier.com/protocol/>
- headscale（自建协调服务器）：<https://github.com/juanfont/headscale>
- NAT1 穿透实践与 ISP 现状：<https://github.com/Guation/nat1_traversal>
- Linux Full Cone NAT 仿真件：<https://github.com/Chion82/netfilter-full-cone-nat> 、
  einat-ebpf：<https://eh5.me/zh-cn/blog/einat-introduction/>
- STUN 测试工具 go-stun：<https://github.com/ccding/go-stun>
