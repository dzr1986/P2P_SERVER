# EasyTier 文档目录

来源：[https://www.easytier.cn/](https://www.easytier.cn/)
官方源码：[EasyTier/easytier.github.io](https://github.com/EasyTier/easytier.github.io)

本地阅读：
- Markdown 源：`official-source/`
- 离线网页：`site-mirror/`（用浏览器打开对应 `.html`）

## 首页与工具

| 页面 | Markdown | 离线 HTML | 线上 |
| --- | --- | --- | --- |
| 中文首页 | [index.md](official-source/index.md) | [index.html](site-mirror/index.html) | https://www.easytier.cn/ |
| 英文首页 | [en/index.md](official-source/en/index.md) | [en/index.html](site-mirror/en/index.html) | https://www.easytier.cn/en/ |
| Web 控制台说明 | [web/index.md](official-source/web/index.md) | [web/index.html](site-mirror/web/index.html) | https://www.easytier.cn/web/ |
| 配置助手 | — | [assistant/index.html](site-mirror/assistant/index.html) | https://www.easytier.cn/assistant/ |

## 简体中文文档

### 开始

| 标题 | Markdown | 离线 HTML |
| --- | --- | --- |
| EasyTier 简介 | [guide/introduction.md](official-source/guide/introduction.md) | [guide/introduction.html](site-mirror/guide/introduction.html) |
| 下载 | [guide/download.md](official-source/guide/download.md) | [guide/download.html](site-mirror/guide/download.html) |
| 安装 (命令行程序) | [guide/installation.md](official-source/guide/installation.md) | [guide/installation.html](site-mirror/guide/installation.html) |
| 安装 (图形界面) | [guide/installation_gui.md](official-source/guide/installation_gui.md) | [guide/installation_gui.html](site-mirror/guide/installation_gui.html) |
| 常见问题 | [guide/faq.md](official-source/guide/faq.md) | [guide/faq.html](site-mirror/guide/faq.html) |

### 命令行工具组网

| 标题 | Markdown | 离线 HTML |
| --- | --- | --- |
| 通过命令行组网 | [guide/networking.md](official-source/guide/networking.md) | [guide/networking.html](site-mirror/guide/networking.html) |
| 快速组网 | [guide/network/quick-networking.md](official-source/guide/network/quick-networking.md) | [guide/network/quick-networking.html](site-mirror/guide/network/quick-networking.html) |
| 去中心组网 | [guide/network/decentralized-networking.md](official-source/guide/network/decentralized-networking.md) | [guide/network/decentralized-networking.html](site-mirror/guide/network/decentralized-networking.html) |
| 使用 Web 控制台 | [guide/network/web-console.md](official-source/guide/network/web-console.md) | [guide/network/web-console.html](site-mirror/guide/network/web-console.html) |
| 使用 WireGuard 客户端接入 | [guide/network/use-easytier-with-wireguard-client.md](official-source/guide/network/use-easytier-with-wireguard-client.md) | [guide/network/use-easytier-with-wireguard-client.html](site-mirror/guide/network/use-easytier-with-wireguard-client.html) |
| 子网代理 | [guide/network/point-to-networking.md](official-source/guide/network/point-to-networking.md) | [guide/network/point-to-networking.html](site-mirror/guide/network/point-to-networking.html) |
| KCP 代理 | [guide/network/kcp-proxy.md](official-source/guide/network/kcp-proxy.md) | [guide/network/kcp-proxy.html](site-mirror/guide/network/kcp-proxy.html) |
| 安全模式（Secure Mode） | [guide/network/secure-mode.md](official-source/guide/network/secure-mode.md) | [guide/network/secure-mode.html](site-mirror/guide/network/secure-mode.html) |
| 网对网 | [guide/network/network-to-network.md](official-source/guide/network/network-to-network.md) | [guide/network/network-to-network.html](site-mirror/guide/network/network-to-network.html) |
| 无 TUN 模式 （免 Root 权限） | [guide/network/no-root.md](official-source/guide/network/no-root.md) | [guide/network/no-root.html](site-mirror/guide/network/no-root.html) |
| SOCKS5 | [guide/network/socks5.md](official-source/guide/network/socks5.md) | [guide/network/socks5.html](site-mirror/guide/network/socks5.html) |
| 搭建共享节点 | [guide/network/host-public-server.md](official-source/guide/network/host-public-server.md) | [guide/network/host-public-server.html](site-mirror/guide/network/host-public-server.html) |
| P2P 优化 | [guide/network/p2p-optimize.md](official-source/guide/network/p2p-optimize.md) | [guide/network/p2p-optimize.html](site-mirror/guide/network/p2p-optimize.html) |
| 魔法 DNS | [guide/network/magic-dns.md](official-source/guide/network/magic-dns.md) | [guide/network/magic-dns.html](site-mirror/guide/network/magic-dns.html) |
| Easytier ACL 功能指南 🛡️ | [guide/config/acl.md](official-source/guide/config/acl.md) | [guide/config/acl.html](site-mirror/guide/config/acl.html) |
| 一键注册服务 | [guide/network/oneclick-install-as-service.md](official-source/guide/network/oneclick-install-as-service.md) | [guide/network/oneclick-install-as-service.html](site-mirror/guide/network/oneclick-install-as-service.html) |
| 安装为 Windows 服务 | [guide/network/install-as-a-windows-service.md](official-source/guide/network/install-as-a-windows-service.md) | [guide/network/install-as-a-windows-service.html](site-mirror/guide/network/install-as-a-windows-service.html) |
| 将服务安装为 Linux Systemd 服务 | [guide/network/install-as-a-systemd-service.md](official-source/guide/network/install-as-a-systemd-service.md) | [guide/network/install-as-a-systemd-service.html](site-mirror/guide/network/install-as-a-systemd-service.html) |
| 安装为 macOS 服务 | [guide/network/install-as-a-macos-service.md](official-source/guide/network/install-as-a-macos-service.md) | [guide/network/install-as-a-macos-service.html](site-mirror/guide/network/install-as-a-macos-service.html) |
| 完整配置选项 | [guide/network/configurations.md](official-source/guide/network/configurations.md) | [guide/network/configurations.html](site-mirror/guide/network/configurations.html) |
| 配置文件 | [guide/network/config-file.md](official-source/guide/network/config-file.md) | [guide/network/config-file.html](site-mirror/guide/network/config-file.html) |

### 图形界面 GUI 组网

| 标题 | Markdown | 离线 HTML |
| --- | --- | --- |
| 图形界面 GUI 组网 | [guide/gui/index.md](official-source/guide/gui/index.md) | [guide/gui/index.html](site-mirror/guide/gui/index.html) |
| 快速组网 | [guide/gui/basic.md](official-source/guide/gui/basic.md) | [guide/gui/basic.html](site-mirror/guide/gui/basic.html) |
| WireGuard 接入 | [guide/gui/vpn_portal.md](official-source/guide/gui/vpn_portal.md) | [guide/gui/vpn_portal.html](site-mirror/guide/gui/vpn_portal.html) |
| 子网代理 | [guide/gui/subnet_proxy.md](official-source/guide/gui/subnet_proxy.md) | [guide/gui/subnet_proxy.html](site-mirror/guide/gui/subnet_proxy.html) |
| [EasyTier 管理器](https://github.com/xlc520/easytier-manager) | [guide/gui/easytier-manager.md](official-source/guide/gui/easytier-manager.md) | [guide/gui/easytier-manager.html](site-mirror/guide/gui/easytier-manager.html) |
| [EasyTier 游戏联机启动器](https://github.com/EasyTier/EasytierGame) | [guide/gui/easytier-game.md](official-source/guide/gui/easytier-game.md) | [guide/gui/easytier-game.html](site-mirror/guide/gui/easytier-game.html) |
| AstralGame 游戏联机工具 | [guide/gui/astral-game.md](official-source/guide/gui/astral-game.md) | [guide/gui/astral-game.html](site-mirror/guide/gui/astral-game.html) |
| [EasyTier 鸿蒙版](https://appgallery.huawei.com/app/detail?id=top.frankhan.easytier&channelId=SHARE&source=appshare) | [guide/gui/easytier-harmonyos.md](official-source/guide/gui/easytier-harmonyos.md) | [guide/gui/easytier-harmonyos.html](site-mirror/guide/gui/easytier-harmonyos.html) |
| QtEasyTier | [guide/gui/qteasytier.md](official-source/guide/gui/qteasytier.md) | [guide/gui/qteasytier.html](site-mirror/guide/gui/qteasytier.html) |
| **MCTier** | [guide/gui/mctier.md](official-source/guide/gui/mctier.md) | [guide/gui/mctier.html](site-mirror/guide/gui/mctier.html) |

### 其他

| 标题 | Markdown | 离线 HTML |
| --- | --- | --- |
| aboutp2p | [guide/aboutp2p.md](official-source/guide/aboutp2p.md) | [guide/aboutp2p.html](site-mirror/guide/aboutp2p.html) |
| 性能测试 | [guide/perf.md](official-source/guide/perf.md) | [guide/perf.html](site-mirror/guide/perf.html) |
| 路线图 | [guide/roadmap.md](official-source/guide/roadmap.md) | [guide/roadmap.html](site-mirror/guide/roadmap.html) |
| 社区和贡献 | [guide/community-and-contribution.md](official-source/guide/community-and-contribution.md) | [guide/community-and-contribution.html](site-mirror/guide/community-and-contribution.html) |
| EasyTier 隐私政策 | [guide/privacy.md](official-source/guide/privacy.md) | [guide/privacy.html](site-mirror/guide/privacy.html) |
| 许可证 | [guide/license.md](official-source/guide/license.md) | [guide/license.html](site-mirror/guide/license.html) |
| 联系方式 | [guide/contact.md](official-source/guide/contact.md) | [guide/contact.html](site-mirror/guide/contact.html) |

## 英文文档

英文页与中文一一对应，位于 `official-source/en/guide/` 与 `site-mirror/en/guide/`。

| 标题 | Markdown | 离线 HTML |
| --- | --- | --- |
| Community and Contribution | [en/guide/community-and-contribution.md](official-source/en/guide/community-and-contribution.md) | [en/guide/community-and-contribution.html](site-mirror/en/guide/community-and-contribution.html) |
| Easytier ACL Feature Guide 🛡️ | [en/guide/config/acl.md](official-source/en/guide/config/acl.md) | [en/guide/config/acl.html](site-mirror/en/guide/config/acl.html) |
| Contact Information | [en/guide/contact.md](official-source/en/guide/contact.md) | [en/guide/contact.html](site-mirror/en/guide/contact.html) |
| Download | [en/guide/download.md](official-source/en/guide/download.md) | [en/guide/download.html](site-mirror/en/guide/download.html) |
| Frequently Asked Questions | [en/guide/faq.md](official-source/en/guide/faq.md) | [en/guide/faq.html](site-mirror/en/guide/faq.html) |
| AstralGame - Game Networking Tool | [en/guide/gui/astral-game.md](official-source/en/guide/gui/astral-game.md) | [en/guide/gui/astral-game.html](site-mirror/en/guide/gui/astral-game.html) |
| Quick Networking | [en/guide/gui/basic.md](official-source/en/guide/gui/basic.md) | [en/guide/gui/basic.html](site-mirror/en/guide/gui/basic.html) |
| [EasyTier Game Launcher](https://github.com/EasyTier/EasytierGame) | [en/guide/gui/easytier-game.md](official-source/en/guide/gui/easytier-game.md) | [en/guide/gui/easytier-game.html](site-mirror/en/guide/gui/easytier-game.html) |
| [EasyTier Manager](https://github.com/xlc520/easytier-manager) | [en/guide/gui/easytier-manager.md](official-source/en/guide/gui/easytier-manager.md) | [en/guide/gui/easytier-manager.html](site-mirror/en/guide/gui/easytier-manager.html) |
| Graphical User Interface (GUI) Networking | [en/guide/gui/index.md](official-source/en/guide/gui/index.md) | [en/guide/gui/index.html](site-mirror/en/guide/gui/index.html) |
| QtEasyTier | [en/guide/gui/qteasytier.md](official-source/en/guide/gui/qteasytier.md) | [en/guide/gui/qteasytier.html](site-mirror/en/guide/gui/qteasytier.html) |
| Subnet Proxy | [en/guide/gui/subnet_proxy.md](official-source/en/guide/gui/subnet_proxy.md) | [en/guide/gui/subnet_proxy.html](site-mirror/en/guide/gui/subnet_proxy.html) |
| WireGuard Access | [en/guide/gui/vpn_portal.md](official-source/en/guide/gui/vpn_portal.md) | [en/guide/gui/vpn_portal.html](site-mirror/en/guide/gui/vpn_portal.html) |
| Installation (Command Line Program) | [en/guide/installation.md](official-source/en/guide/installation.md) | [en/guide/installation.html](site-mirror/en/guide/installation.html) |
| Installation (Graphical Interface) | [en/guide/installation_gui.md](official-source/en/guide/installation_gui.md) | [en/guide/installation_gui.html](site-mirror/en/guide/installation_gui.html) |
| Introduction | [en/guide/introduction.md](official-source/en/guide/introduction.md) | [en/guide/introduction.html](site-mirror/en/guide/introduction.html) |
| License | [en/guide/license.md](official-source/en/guide/license.md) | [en/guide/license.html](site-mirror/en/guide/license.html) |
| Configuration File | [en/guide/network/config-file.md](official-source/en/guide/network/config-file.md) | [en/guide/network/config-file.html](site-mirror/en/guide/network/config-file.html) |
| Complete Configuration Options | [en/guide/network/configurations.md](official-source/en/guide/network/configurations.md) | [en/guide/network/configurations.html](site-mirror/en/guide/network/configurations.html) |
| Decentralized Networking | [en/guide/network/decentralized-networking.md](official-source/en/guide/network/decentralized-networking.md) | [en/guide/network/decentralized-networking.html](site-mirror/en/guide/network/decentralized-networking.html) |
| Setting Up a Shared Node | [en/guide/network/host-public-server.md](official-source/en/guide/network/host-public-server.md) | [en/guide/network/host-public-server.html](site-mirror/en/guide/network/host-public-server.html) |
| Install as a macOS Service | [en/guide/network/install-as-a-macos-service.md](official-source/en/guide/network/install-as-a-macos-service.md) | [en/guide/network/install-as-a-macos-service.html](site-mirror/en/guide/network/install-as-a-macos-service.html) |
| Install the Service as a Linux Systemd Service | [en/guide/network/install-as-a-systemd-service.md](official-source/en/guide/network/install-as-a-systemd-service.md) | [en/guide/network/install-as-a-systemd-service.html](site-mirror/en/guide/network/install-as-a-systemd-service.html) |
| Install as a Windows Service | [en/guide/network/install-as-a-windows-service.md](official-source/en/guide/network/install-as-a-windows-service.md) | [en/guide/network/install-as-a-windows-service.html](site-mirror/en/guide/network/install-as-a-windows-service.html) |
| KCP Proxy | [en/guide/network/kcp-proxy.md](official-source/en/guide/network/kcp-proxy.md) | [en/guide/network/kcp-proxy.html](site-mirror/en/guide/network/kcp-proxy.html) |
| Magic DNS | [en/guide/network/magic-dns.md](official-source/en/guide/network/magic-dns.md) | [en/guide/network/magic-dns.html](site-mirror/en/guide/network/magic-dns.html) |
| Network to Network | [en/guide/network/network-to-network.md](official-source/en/guide/network/network-to-network.md) | [en/guide/network/network-to-network.html](site-mirror/en/guide/network/network-to-network.html) |
| No TUN Mode (No Root Permission Required) | [en/guide/network/no-root.md](official-source/en/guide/network/no-root.md) | [en/guide/network/no-root.html](site-mirror/en/guide/network/no-root.html) |
| One-Click Register Service | [en/guide/network/oneclick-install-as-service.md](official-source/en/guide/network/oneclick-install-as-service.md) | [en/guide/network/oneclick-install-as-service.html](site-mirror/en/guide/network/oneclick-install-as-service.html) |
| P2P Optimization | [en/guide/network/p2p-optimize.md](official-source/en/guide/network/p2p-optimize.md) | [en/guide/network/p2p-optimize.html](site-mirror/en/guide/network/p2p-optimize.html) |
| Subnet Proxy (Point-to-Network) | [en/guide/network/point-to-networking.md](official-source/en/guide/network/point-to-networking.md) | [en/guide/network/point-to-networking.html](site-mirror/en/guide/network/point-to-networking.html) |
| Quick Networking | [en/guide/network/quick-networking.md](official-source/en/guide/network/quick-networking.md) | [en/guide/network/quick-networking.html](site-mirror/en/guide/network/quick-networking.html) |
| Secure Mode | [en/guide/network/secure-mode.md](official-source/en/guide/network/secure-mode.md) | [en/guide/network/secure-mode.html](site-mirror/en/guide/network/secure-mode.html) |
| SOCKS5 | [en/guide/network/socks5.md](official-source/en/guide/network/socks5.md) | [en/guide/network/socks5.html](site-mirror/en/guide/network/socks5.html) |
| Connect Using WireGuard Client | [en/guide/network/use-easytier-with-wireguard-client.md](official-source/en/guide/network/use-easytier-with-wireguard-client.md) | [en/guide/network/use-easytier-with-wireguard-client.html](site-mirror/en/guide/network/use-easytier-with-wireguard-client.html) |
| Using the Web Console | [en/guide/network/web-console.md](official-source/en/guide/network/web-console.md) | [en/guide/network/web-console.html](site-mirror/en/guide/network/web-console.html) |
| Networking | [en/guide/networking.md](official-source/en/guide/networking.md) | [en/guide/networking.html](site-mirror/en/guide/networking.html) |
| Performance Testing | [en/guide/perf.md](official-source/en/guide/perf.md) | [en/guide/perf.html](site-mirror/en/guide/perf.html) |
| EasyTier Privacy Policy | [en/guide/privacy.md](official-source/en/guide/privacy.md) | [en/guide/privacy.html](site-mirror/en/guide/privacy.html) |
| Roadmap | [en/guide/roadmap.md](official-source/en/guide/roadmap.md) | [en/guide/roadmap.html](site-mirror/en/guide/roadmap.html) |
| https://vitepress.dev/reference/default-theme-home-page | [en/index.md](official-source/en/index.md) | [en/index.html](site-mirror/en/index.html) |
| index | [en/web/index.md](official-source/en/web/index.md) | [en/web/index.html](site-mirror/en/web/index.html) |
