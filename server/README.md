# p2p_server — 按 EasyTier 目录拆的信令核

这是 **自己的** NatServer（原 `p2p_server`），不是客户端 SDK。
对照 EasyTier `easytier-core/src/` 的模块名拆目录，方便一层一层学。
产品仍是「有中心信令的 IoT UID」，**不是** SD-WAN。

```
ls server
  config/         配置（EasyTier: config）
  peers/          注册表（EasyTier: peers；只做 UUID→地址，不做 mesh 路由）
  connectivity/   STUN responder + 映射观察 + NAT 探测（EasyTier: stun/responder+collector）
  management/     防滥用、白名单、/metrics（EasyTier: management）
  rpc/            报文分发 on_msg_*（EasyTier: rpc）
  listener/       UDP bind + LocalListeners 防回环（EasyTier: listener）
  instance/       生命周期 + epoll 收包（EasyTier: instance）
  proxyserver/    专用中继（对标 DERP，不是 gateway）
  wakeserver/     IoT 唤醒（他们没有对等物）
```

依赖：这些目录用 `core/foundation|socket|packet`，不要反向去 include `core/instance`（那是客户端门面）。

**不建**：`gateway/`、`peers/route`、`wasi/`。节点不为别人转发。

旧路径 `server/natserver/src/*.h` 只是转发头。二进制仍输出到 `server/natserver/bin/p2p_natserver`。
