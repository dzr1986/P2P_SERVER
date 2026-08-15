# C++ 风格重构指南

> 目标：把代码库从「能跑的 C 风格 C++」演进为「易维护的现代 C++17」，
> 服务于《P2P服务器开发计划书》的长期迭代。
> 原则：**行为不变、协议不变、测试全绿**（`test.sh` 44 项 + 单测）为每次重构的硬约束。

---

## 1. 风格规范（增量适用：改到哪个文件，哪个文件遵守）

### 1.1 资源管理
- **RAII 优先**：fd/内存/锁一律用对象生命周期管理。
  - 服务端 UDP socket → `common/Net.h::UdpFd`（move-only，析构自动 close）
  - 客户端 socket → `client/sdk/transport/UdpSocket.h`（已支持移动语义）
  - 堆对象 → `std::unique_ptr`（如 `create_encryptor` 已返回 unique_ptr）
- 禁止裸 `new/delete`、裸 `socket()/close()` 散落业务代码。

### 1.2 报文解析与构造
- 解析用 `common/Packet.h::PacketReader`：越界返回 false，**禁止先 memcpy 后检查**。
- 构包用 `PacketWriter`：容量检查与写入合一。
- 定长 char 数组字段转 `std::string` 用 `wire_str()`；
  反向填充用 `Util.h::copy_str_field()`（补零 + 截断，不越界读）。
- wire 结构体保持 `#pragma pack(1)` + `memcpy`（跨平台安全，不做指针强转读取；
  只读转发场景的 `reinterpret_cast` 需注明理由）。

### 1.3 函数与类
- **单一职责**：巨型 switch 拆为 `on_msg_*` 私有方法（RecvProcess/P2PProxy 已完成），
  每个处理器可独立阅读与测试。
- 常量用 `constexpr` + `kCamelCase` 命名，放匿名命名空间；禁止裸魔法数。
- 类禁止拷贝时显式 `= delete`；默认成员初始化（`{}`）代替构造函数体赋值。
- 聚合零初始化用 `T v{};` 代替 `memset(&v, 0, sizeof(v))`（新代码强制，存量渐进替换）。

### 1.4 并发
- 锁的粒度与共享数据放在一起声明并注释保护关系（见 `P2PProxy.h` 注释）。
- 读多写少用 `std::shared_mutex`；跨容器一致性优先「短临界区 + 快照」。
- 原子计数用 `std::atomic`，不要用锁保护单个计数器。

### 1.5 错误处理
- SDK 边界维持 C 风格错误码（嵌入式/多语言绑定友好），内部实现允许早退 + 日志。
- 安全失败必须显式（如 `p2p_random_bytes` 返回 -1 即拒绝服务，不降级）。

---

## 2. 已完成的重构（本分支）

| 模块 | 改动 |
|------|------|
| `common/Net.h`（新增） | `UdpFd`：服务端 UDP socket RAII 封装（open/reuse/bind/收发） |
| `common/Packet.h`（新增） | `PacketReader`/`PacketWriter` 边界检查读写器 + `wire_str()` |
| `server/proxyserver/*` | 示范模块全量重构：RAII socket（消灭手动 close 路径）、巨型 switch 拆为 5 个 `on_msg_*` 处理器、`PacketReader` 解析、常量命名规范化、聚合初始化 |
| `server/natserver/RecvProcess.cpp` | 550 行 switch 拆为 17 个 `on_msg_*` 处理器（声明见 `NatServer.h`），全部改用 `PacketReader`/`copy_str_field`，日志统一 `addr_to_str` |
| `client/peer.cpp`（删除） | 未参与构建的早期裸协议 demo（393 行死代码），文档同步更新 |

## 2.1 第二批已完成

| 模块 | 改动 |
|------|------|
| `common/Net.h` | 新增 `EpollFd`（epoll RAII）；`UdpFd` 增加 `local_port()`/`local_addr()` |
| `common/File.h`（新增） | `FileHandle`（`unique_ptr<FILE, &fclose>`）+ `open_file`/`read_file_all`，消灭「每条错误路径手写 fclose」 |
| `server/natserver/NatTypeCheck` | 主/备双 socket 迁移 `UdpFd`（析构自动关闭）；报文构造/解析改 `PacketWriter`/`PacketReader` |
| `server/natserver/NatServer.cpp` | `recv_socks_` 迁移 `vector<UdpFd>`；`recv_thread` 的 epoll 改 `EpollFd`（所有退出路径自动关闭）；`send_msg` 改 `PacketWriter` |
| `AntiAbuse`/`LicenseMgr`/`CfgFile` | 文件 IO 全部 `FileHandle` RAII 化，load 统一 `read_file_all` |
| `client/sdk/api/P2PClient` | 清理全部编译警告（聚合初始化 `{}`、未用参数），全仓 `-Wall -Wextra` 零警告 |

## 2.2 第三批已完成（路线收官）

| 模块 | 改动 |
|------|------|
| `client/sdk/api/P2PClient` | `connecting/connected` 双布尔收拢为 `enum class ConnState`（Idle → Connecting → Connected）；连接建立转移集中到唯一入口 `set_connected()`（原先散落 4 处的「置位 + 回调」模式）；`via_relay` 记录路径类型 |
| `server/natserver/NatServer.cpp` | `run()` 线程编排拆分为 `start_threads()`/`stop_threads()`（`RunThreads` 线程组结构）；StatusServer 停机唤醒收拢为 `wake_tcp_accept()` 辅助函数 |
| `common/Packet.h` | 新增 `build_msg()` 统一 XN 报文构造，消灭 NatServer/P2PProxy/NatTypeCheck 三处手写 `MsgHead` |
| `server/natserver/CfgFile.cpp` | `parse_value` 巨型 if-else 链改为表驱动（key → setter 函数指针表），新增配置项只需加一行；抽取 `to_bool`/`split_csv`/`set_int_in_range` 辅助 |
| `common/Log.h` | `log()` 增加 `__attribute__((format(printf)))` 编译期格式串检查（全量重编零告警，存量格式串全部正确） |

### 决策记录：时间类型保留 `uint64_t ms`

原路线中「`std::chrono` 收敛」项评估后决定**不做**：
- SDK 已有单一时钟源 `plat_now_ms()`（steady_clock 毫秒），所有超时/重传/心跳共用，无混用歧义；
- 换成 `std::chrono::time_point/duration` 需触及 Session/P2PClient/NatDetect 全部计时点，
  属纯类型改写，无行为收益，且增加嵌入式移植成本（`Plat.h` 是唯一平台隔离点）；
- 服务端侧 `time_t`（秒级 TTL）与毫秒时钟职责不同，保留现状。

## 3. 后续可选项（非必须）

| 模块 | 说明 |
|------|------|
| `common/ProtoDef.h` | 若协议扩展频繁，可进一步引入类型安全的消息注册表（msg_id → 结构体映射，编译期校验） |
| 客户端 SDK | 计划书 P5 前向保密握手 / P6 集群调度（新增代码直接按本指南规范编写） |

## 3.1 P2/P4 通道 API 层（新增，按本指南编写）

| 模块 | 改动 |
|------|------|
| `client/sdk/iotc/IOTC.*` | C 风格 SID/通道 API；RAII 单例；回调与 API 锁序约定 |
| `client/sdk/iotc/AvCodec.h` | 头文件可单测的帧分片/重组 + too-late-drop |
| `client/sdk/iotc/AVAPIs.*` | 帧级收发 + IOCtrl + 码率建议 |
| `client/sdk/iotc/RDTAPIs.*` | 可靠字节流（分片 + leftover） |
| `client/sdk/iotc/TunnelCodec.h` + `P2PTunnelAPIs.*` | TCP 端口映射；`TcpFd` RAII |
| `common/Net.h` | 新增 `TcpFd`（move-only，析构 close） |
| `client/demo/iotc_demo.cpp` | 四通道 + 隧道验收程序 |
| `tests/av_frame_test.cpp` | AvCodec / TunnelCodec 单测 |
| `common/X25519.*` + `Handshake.h` | P5 前向保密：RFC 7748 ECDH + 握手编解码 |
| `P2PProxy` | 每 UID 中继配额（超限丢包） |

## 4. 验证清单（每批重构必跑）

```sh
make -s all            # 零 error（新增代码零 warning）
./tests/bin/crypto_test
./tests/bin/session_test
./tests/bin/uid_test
./tests/bin/av_frame_test
bash test.sh           # 端到端全绿（含 [10] IOTC/AV/RDT/Tunnel）
```
