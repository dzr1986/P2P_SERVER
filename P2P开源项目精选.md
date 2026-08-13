# 网上最硬的 P2P 开源项目精选

网上没有「唯一最强」的 P2P 仓库。按工程含金量和生产验证，可以很明确地排出几档。

**一句话结论：**

- 协议广度看 **libp2p**
- 连通性看 **iroh**
- 文件分发看 **libtorrent**

这三份是目前开源圈里最硬的 P2P 代码。

---

## 怎么选

| 你想做什么 | 直接看这个 |
|---|---|
| 学「完整 P2P 协议栈」 | **libp2p**（先读 specs，再读 go-libp2p） |
| 做「能穿透 NAT 的直连」 | **iroh** |
| 做 BT / 大文件分发 | **libtorrent** + qBittorrent |
| 做浏览器 P2P | **WebTorrent** 或 **js-libp2p** |
| 做音视频 / DataChannel | **Pion WebRTC** |
| 做文件夹同步产品 | **Syncthing** |
| 做内容寻址网络 | **Kubo / IPFS** |

---

## 第一档：真正能当「业界标准」的

### 1. libp2p —— 综合最强

- 官网：<https://libp2p.io>
- 规格：<https://github.com/libp2p/libp2p>
- 实现：
  - [go-libp2p](https://github.com/libp2p/go-libp2p) ~6.8k stars
  - [rust-libp2p](https://github.com/libp2p/rust-libp2p) ~5.6k stars
  - [js-libp2p](https://github.com/libp2p/js-libp2p) ~2.6k stars

它从 IPFS 拆出来，现在是模块化 P2P 网络栈：传输（TCP / QUIC / WebRTC / WebTransport）、加密握手、多路复用、NAT 打洞、DHT、PubSub、中继。IPFS、Ethereum、Filecoin 都在用。

**适合：** 做去中心化应用、区块链、内容寻址网络。

**代价：** 概念多、配置重，NAT 打洞成功率不如 iroh 那种「有中继兜底」的方案。

**多语言实现：**

- [go-libp2p](https://github.com/libp2p/go-libp2p)
- [js-libp2p](https://github.com/libp2p/js-libp2p)（Node.js 和浏览器）
- [rust-libp2p](https://github.com/libp2p/rust-libp2p)（也支持 Wasm）
- [py-libp2p](https://github.com/libp2p/py-libp2p)
- [cpp-libp2p](https://github.com/libp2p/cpp-libp2p)
- [swift-libp2p](https://github.com/swift-libp2p/swift-libp2p)
- [nim-libp2p](https://github.com/vacp2p/nim-libp2p)
- [jvm-libp2p](https://github.com/libp2p/jvm-libp2p)
- [dotnet-libp2p](https://github.com/NethermindEth/dotnet-libp2p)
- [litep2p](https://github.com/paritytech/litep2p)（Parity 的轻量 Rust 替代实现）

### 2. iroh —— 近年最「能打」的新栈

- 仓库：<https://github.com/n0-computer/iroh> ~1 万 stars，已到 1.0
- 官网：<https://iroh.computer>
- 对比文章：[Comparing Iroh & Libp2p](https://www.iroh.computer/blog/comparing-iroh-and-libp2p)

口号是：**别拨 IP，拨公钥。** 用 QUIC + 打洞 + 公共中继兜底，思路接近 Tailscale。官方对比里，libp2p 打洞大约 70%，iroh 更强调「连不上就走中继，保证能通」。

**适合：** 要「两台设备在家宽 NAT 后面也能连上」的应用。

**语言：** Rust。生产里已经跑在大量设备上。

**仓库结构要点：**

- `iroh`：打洞与中继通信核心库
- `iroh-relay`：中继客户端 / 服务端（线上公共中继也是这份代码）
- `iroh-base`：`EndpointId`、`RelayUrl` 等基础类型
- `iroh-dns-server`：用 DNS / Pkarr 查找 EndpointId

周边工具：

- [sendme](https://github.com/n0-computer/sendme)：基于 iroh 发文件 / 目录
- [dumbpipe](https://github.com/n0-computer/dumbpipe)：设备之间的 Unix pipe

### 3. libtorrent —— BitTorrent 引擎天花板

- 仓库：<https://github.com/arvidn/libtorrent> ~6k stars
- 官网：<https://www.libtorrent.org>

Arvid Norberg 写的 C++ 引擎，qBittorrent、Deluge 都靠它。CPU / 内存效率、协议完整度、嵌入式到服务器都能跑，是「协议实现」这一类里最硬的代码。

**目标：**

- CPU 效率高
- 内存占用低
- API 好用
- 协议扩展完整，适合真实部署

---

## 第二档：完整产品，代码也值得读

| 项目 | Stars | 为什么牛 | 链接 |
|---|---|---|---|
| **Syncthing** | ~8.8 万 | 开源 P2P 文件同步里最成功的产品，打洞、中继、设备发现都齐 | [syncthing/syncthing](https://github.com/syncthing/syncthing) |
| **qBittorrent** | ~3.8 万 | 最强开源 BT 客户端，底层就是 libtorrent | [qbittorrent/qBittorrent](https://github.com/qbittorrent/qBittorrent) |
| **WebTorrent** | ~3.1 万 | 浏览器里跑 BitTorrent，WebRTC 做传输 | [webtorrent/webtorrent](https://github.com/webtorrent/webtorrent) |
| **Kubo (IPFS)** | ~1.7 万 | 内容寻址 + Bitswap + DHT，libp2p 的旗舰应用 | [ipfs/kubo](https://github.com/ipfs/kubo) |
| **Pion WebRTC** | ~1.7 万 | 纯 Go 的 WebRTC，音视频 / DataChannel / ICE 全套 | [pion/webrtc](https://github.com/pion/webrtc) |

### Syncthing

开源持续文件同步。没有中心服务器也能在设备之间同步目录，NAT 打洞、中继、设备发现都做完了。想看「完整 P2P 产品」怎么落地，优先读这份。

### qBittorrent

C++ / Qt 客户端，引擎是 libtorrent。想看 BT 客户端产品层怎么包一层成熟引擎，看它。

### WebTorrent

同一套 JavaScript 同时跑在 Node.js 和浏览器里。浏览器侧用 WebRTC DataChannel 做传输，是「网页里做 P2P」的经典样本。

注意：浏览器里的 WebTorrent 节点只能连同样支持 WebTorrent / WebRTC 的对端，不能直接连所有传统 BT 客户端。

### Kubo（IPFS）

第一个、也是目前用得最多的 IPFS 实现。内容寻址（CID）、UnixFS、Bitswap、Amino DHT、HTTP Gateway 都在这里。想理解「内容寻址网络」怎么跑，读 Kubo + libp2p。

### Pion WebRTC

纯 Go 实现 WebRTC API，无 Cgo。覆盖 DataChannel、音视频收发、完整 ICE Agent、STUN / TURN、Trickle ICE。做实时通信或浏览器外的 WebRTC 节点，这是 Go 生态的事实标准。

---

## 第三档：专项很强，别漏

### NAT 打洞 / ICE

- **[libjuice](https://github.com/paullouisageneau/libjuice)**：无依赖 C 实现 ICE / STUN / TURN，打洞库里很干净，适合嵌入式和原生应用。
- **[pion/ice](https://github.com/pion/ice)**：Go 里的 ICE 事实标准，Pion WebRTC 和不少 mesh VPN 都用它。
- **Tailscale magicsock / DERP**：打洞工程上最强之一。客户端开源，协调服务不完全开源。iroh 明确说学过它。

### 轻量 / 替代实现

- **[litep2p](https://github.com/paritytech/litep2p)**：Parity 的轻量 Rust libp2p 替代实现。
- **[libp2p-iroh](https://github.com/hashcashier/libp2p-iroh)**：把 iroh 的 QUIC 连通性接到 libp2p Transport 上，用 PeerId 拨号，NAT 后面也能连。

---

## 推荐阅读顺序

1. 先读 [libp2p specs](https://github.com/libp2p/specs)，建立「传输 / 安全 / 多路复用 / 发现 / 中继」的分层概念。
2. 再读 [go-libp2p](https://github.com/libp2p/go-libp2p) 或 [rust-libp2p](https://github.com/libp2p/rust-libp2p) 的 examples，看这些层怎么拼起来。
3. 对照 [iroh](https://github.com/n0-computer/iroh) 和 [Comparing Iroh & Libp2p](https://www.iroh.computer/blog/comparing-iroh-and-libp2p)，理解「功能全集」和「连通性优先」两条路线。
4. 如果关心文件分发，读 [libtorrent](https://github.com/arvidn/libtorrent) 文档和 [qBittorrent](https://github.com/qbittorrent/qBittorrent) 的引擎封装。
5. 如果关心浏览器，读 [WebTorrent](https://github.com/webtorrent/webtorrent) 和 [js-libp2p WebRTC 文档](https://libp2p.io/docs/webrtc/)。

---

## 星标速查（约 2026 年 8 月）

| 仓库 | Stars | 语言 | 定位 |
|---|---|---|---|
| [syncthing/syncthing](https://github.com/syncthing/syncthing) | ~87,600 | Go | P2P 文件同步产品 |
| [qbittorrent/qBittorrent](https://github.com/qbittorrent/qBittorrent) | ~38,300 | C++ | BT 客户端 |
| [webtorrent/webtorrent](https://github.com/webtorrent/webtorrent) | ~31,000 | JavaScript | 浏览器 BT |
| [ipfs/kubo](https://github.com/ipfs/kubo) | ~17,100 | Go | IPFS 节点 |
| [pion/webrtc](https://github.com/pion/webrtc) | ~16,700 | Go | WebRTC 实现 |
| [n0-computer/iroh](https://github.com/n0-computer/iroh) | ~10,000 | Rust | 打洞优先的网络栈 |
| [libp2p/go-libp2p](https://github.com/libp2p/go-libp2p) | ~6,800 | Go | libp2p 主力实现 |
| [arvidn/libtorrent](https://github.com/arvidn/libtorrent) | ~6,000 | C++ | BT 引擎 |
| [libp2p/rust-libp2p](https://github.com/libp2p/rust-libp2p) | ~5,600 | Rust | libp2p Rust 实现 |
| [libp2p/libp2p](https://github.com/libp2p/libp2p) | ~3,100 | — | 规格与总览 |

星标会变，数字只用来横向对比量级，不代表代码质量排序。

---

## 参考链接

- libp2p 官网：<https://libp2p.io>
- libp2p WebRTC：<https://libp2p.io/docs/webrtc/>
- Iroh 官网：<https://iroh.computer>
- Iroh 1.0 回顾：<https://www.iroh.computer/blog/the-road-to-iroh-1-0>
- libtorrent 官网：<https://www.libtorrent.org>
- Syncthing 官网：<https://syncthing.net>
- IPFS 文档：<https://docs.ipfs.tech>
- Pion：<https://pion.ly>
- Tailscale NAT 打洞系列：<https://tailscale.com/blog/nat-traversal-improvements-pt-1>
