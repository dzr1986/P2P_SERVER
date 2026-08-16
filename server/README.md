# p2p_server — 按 EasyTier 目录拆的信令核

这是 **自己的** NatServer（原 `p2p_server`），不是客户端 SDK。
对照 EasyTier `easytier-core/src/` 的模块名拆目录，方便一层一层学。
产品仍是「有中心信令的 IoT UID」，**不是** SD-WAN。

```
ls server
  config/         配置（guide: config-file / configurations）
  peers/          注册表 + RegistrySync 集群（guide: host-public-server；不做 mesh）
  connectivity/   STUN / 映射观察 / NAT 探测（guide: aboutp2p）
  management/     防滥用、JailFile、白名单、AuthChallenge、ConnectAuth、RelayHealth、/metrics
  rpc/            报文分发 on_msg_*
  listener/       UDP bind + LocalListeners 防回环
  instance/       生命周期组合根（只焊模块，不堆同步/择优实现）
  proxyserver/    专用中继（guide：共享节点不转发业务）
  wakeserver/     IoT 唤醒
```

对照表见 [`docs/EasyTier指南架构对照.md`](../docs/EasyTier指南架构对照.md)。
文档地图 [`docs/README.md`](../docs/README.md)；做成流媒体**平台**还缺什么见
[`docs/P2P流媒体服务器能力缺口.md`](../docs/P2P流媒体服务器能力缺口.md)（不要把 Master/SFU/计费堆回本目录）。

依赖：这些目录用 `core/foundation|socket|packet`，不要反向去 include `core/instance`（那是客户端门面）。

**不建**：`gateway/`、`peers/route`、`wasi/`。节点不为别人转发。

旧路径 `server/natserver/src/*.h` 只是转发头。二进制仍输出到 `server/natserver/bin/p2p_natserver`。

配置覆盖顺序（学 EasyTier）：`P2pServers.cfg` < `P2P_*` 环境变量 < 命令行端口。
文件值支持 `${ENV}`；`P2P_DISABLE_ENV_PARSING=1` 只关展开。状态口可用 `StatusAllow` / `P2P_STATUS_ALLOW` 做来源 CIDR 白名单。
`JailFile` 给 fail2ban 一行一个 IP。NatServer 收到 `MSG_PROXY_RELAY_DATA` 直接丢（业务只走 `p2p_proxy`）。
详见 [`docs/EasyTier配置对照.md`](../docs/EasyTier配置对照.md)、[`docs/p2p_server优化完毕.md`](../docs/p2p_server优化完毕.md)。
