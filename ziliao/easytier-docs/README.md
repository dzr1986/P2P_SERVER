# EasyTier 官网文档离线存档

本目录保存 [https://www.easytier.cn/](https://www.easytier.cn/)（文档入口页：[简介](https://www.easytier.cn/guide/introduction.html)）的全部文档内容。

该站是 **VitePress 静态文档站**，官方源码仓库为 [EasyTier/easytier.github.io](https://github.com/EasyTier/easytier.github.io)（Apache-2.0）。  
因此「保存整站」有两条互补路径：**克隆 Markdown 源码**（推荐）和 **镜像线上 HTML**（可离线打开）。

完整页面清单见 [INDEX.md](INDEX.md)，快照信息见 [SNAPSHOT.txt](SNAPSHOT.txt)。

## 目录结构

```
ziliao/easytier-docs/
├── README.md              # 本说明（保存方法 + 使用方式）
├── INDEX.md               # 中英文全部页面索引
├── SNAPSHOT.txt           # 克隆时间与上游 commit
├── official-source/       # 官方 VitePress 源码（Markdown + 图片 + 主题）
├── site-mirror/           # 线上站点 HTML/CSS/JS/图片镜像
└── scripts/
    ├── urls.txt           # 已知页面 URL 清单
    └── mirror-site.sh     # 重新镜像线上站点的脚本
```

## 怎么阅读

- **看正文（推荐）**：直接打开 `official-source/guide/` 下的 `.md`。中文在 `guide/`，英文在 `en/guide/`。
- **按网站样子离线浏览**：用浏览器打开 `site-mirror/index.html`，或 `site-mirror/guide/introduction.html`。
- **配置助手**：`site-mirror/assistant/index.html`（含 wasm）。
- **Web 控制台**：文档说明在 `official-source/web/index.md`；控制台本身是在线服务，静态镜像只能保存入口页。

本地预览官方源码站（需 Node.js / pnpm）：

```bash
cd official-source
pnpm install
pnpm docs:dev
```

## 保存整站的常用方法

下面按「这个站点」的实际情况排序。

### 1. 克隆官方文档仓库（最完整、最干净）

官网内容就是这个仓库里的 Markdown，比抓 HTML 更适合阅读和检索。

```bash
git clone --depth 1 https://github.com/EasyTier/easytier.github.io.git
```

本次存档已按上游 commit `0d8b589eaf6181b1ac958c4489237e203f5f4d02`（2026-08-06）复制到 `official-source/`。

优点：源文件、图片、侧栏结构、中英文对照都在。  
局限：不含已构建的 HTML；`download` 等少数页面有 Vue 脚本，需构建后才有完整交互。

### 2. wget / HTTrack 镜像线上静态页

适合「用浏览器离线打开，长得像原站」。

```bash
# 本仓库已写好脚本
./scripts/mirror-site.sh

# 或手动 wget
wget --mirror --convert-links --adjust-extension --page-requisites \
     --no-parent --domains=www.easytier.cn,easytier.cn \
     -e robots=off -P ./site-mirror \
     https://www.easytier.cn/
```

Windows 上也可用 [HTTrack](https://www.httrack.com/)：工程 URL 填 `https://www.easytier.cn/`，限制只抓本站域名。

优点：打开即用。  
局限：页脚备案图等外链会 404；VitePress 资源带 hash，站点改版后文件名会变。

### 3. 浏览器「整页另存」或扩展

- Chrome / Edge：页面另存为「网页，全部」。
- 扩展：[SingleFile](https://github.com/gildas-lormeau/SingleFile) 把一页打成单个 HTML。
- 扩展：[SingleFileZ](https://github.com/gildas-lormeau/SingleFileZ) / [WebScrapBook](https://addons.mozilla.org/firefox/addon/webscrapbook/)。

适合只存几页。要整站仍需配合方法 1 或 2。

### 4. 互联网档案馆

在 [web.archive.org](https://web.archive.org/) 搜索 `easytier.cn`，可看历史快照，但不能替代本地完整副本。

### 5. 不推荐：无头浏览器整站爬 SPA

本站文档页是静态 HTML，不必上 Playwright/Puppeteer。Web 控制台若是强依赖后端的 SPA，爬下来也只有壳子。

## 本次已保存内容

| 类别 | 数量 | 位置 |
| --- | --- | --- |
| Markdown 文档（含首页/英文） | 87 | `official-source/` |
| 离线 HTML 页面 | 88 | `site-mirror/` |
| 文档配图与站点资源 | 源码约 11MB，镜像约 16MB | `official-source/assets/`、`site-mirror/assets/` |

中文文档覆盖侧栏全部条目：简介、下载、CLI/GUI 安装、FAQ、命令行组网（快速/去中心/Web 控制台/WireGuard/子网代理/KCP/安全模式/网对网/免 Root/SOCKS5/共享节点/P2P 优化/魔法 DNS/ACL/各平台服务）、GUI 组网与第三方客户端、关于 P2P、性能测试、路线图、社区、隐私、许可证、联系方式。英文对应页一并保存。

## 许可与归属

EasyTier 文档以 **Apache License 2.0** 发布，版权归 EasyTier 项目。本目录仅为离线备份与学习对照，未改写原文。完整许可见 `official-source/LICENSE`。
