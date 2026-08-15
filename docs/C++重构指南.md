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

## 3. 待重构路线（后续分批，每批保持测试全绿）

| 优先级 | 模块 | 计划 |
|--------|------|------|
| 高 | `client/sdk/api/P2PClient.cpp` | `Conn` 状态机字段（connecting/connected/have_direct/direct_ok/relay_ok 布尔组合）收拢为显式 `enum class State`，转移集中到单函数 |
| 中 | `server/natserver/NatServer.cpp` | `run()` 线程编排拆分（start/stop helper）；StatusServer 停机唤醒 socket RAII |
| 中 | `common/ProtoDef.h` | 消息构造统一经 `PacketWriter` 辅助函数（`make_msg<T>(id, body)`），逐步消灭调用方手写 `MsgHead` |
| 中 | 时间类型 | `time_t`/`uint64_t ms` 混用收敛为 `std::chrono::steady_clock`（先 SDK 后服务端） |
| 低 | `CfgFile.cpp` | `std::string_view` 解析；配置项表驱动注册 |
| 低 | 日志 | `Log.h` 增加编译期格式检查（`__attribute__((format)))` |

## 4. 验证清单（每批重构必跑）

```sh
make -s all            # 零 error（新增代码零 warning）
./tests/bin/crypto_test
./tests/bin/session_test
bash test.sh           # 44 项端到端全绿
```
