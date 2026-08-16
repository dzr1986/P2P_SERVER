#ifndef P2P_PEER_MANAGE_H
#define P2P_PEER_MANAGE_H

#include <arpa/inet.h>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// 在线对等节点（仿原实现 stru_peer）
// ---------------------------------------------------------------------------
struct Peer {
    std::string uuid;
    sockaddr_in pub_addr;          // 服务端观察到的公网地址
    sockaddr_in lan_addr;          // 上报的私网地址（ip 取报文源，port 取上报）
    uint8_t     dev_type;          // 0=未知 1=设备 2=App/客户端
    uint8_t     nattype;           // 客户端自检 NAT 类型（NatType）
    time_t      last_heartbeat;
    time_t      register_time;
    time_t      auth_expire;       // 鉴权有效期截止（0=无需鉴权/未鉴权）
    uint16_t    extlen;            // 扩展信息长度
    std::string extinfo;           // 扩展信息（上限 MAX_EXTINFO）
    bool        need_p2p = false;  // 心跳 extinfo 含 np=1
};

// ---------------------------------------------------------------------------
// 线程安全的对等表（仿原实现 CPeerManage）
// 读写锁：读多写少（心跳/CONNECT 高频读）
// ---------------------------------------------------------------------------
class PeerManager {
public:
    PeerManager();
    ~PeerManager();

    bool upsert(const std::string& uuid, const sockaddr_in& pub,
                const sockaddr_in& lan, uint8_t dev_type, uint8_t nattype,
                const std::string& extinfo);
    // 注册表同步：写入对端服务器广播的节点（保留源侧心跳时间，鉴权视为已通过）
    bool upsert_synced(const std::string& uuid, const sockaddr_in& pub,
                       const sockaddr_in& lan, uint8_t dev_type, uint8_t nattype,
                       const std::string& extinfo, time_t hb_time);
    bool remove(const std::string& uuid);
    bool exists(const std::string& uuid);
    bool get(const std::string& uuid, Peer& out);

    void set_nattype(const std::string& uuid, uint8_t nattype);
    void set_auth_expire(const std::string& uuid, time_t expire);
    bool authed(const std::string& uuid) const;
    // 清掉过期的鉴权记录（周期调用，防止 auth_sessions_ 无限增长）
    void cleanup_auth_sessions();

    size_t size() const;
    size_t device_count() const;
    size_t client_count() const;
    size_t authed_count() const;
    size_t need_p2p_count() const;

    void cleanup_timeout(int timeout_sec);
    // 清理超时节点并返回被移除的 uuid（供注册表同步广播删除）
    std::vector<std::string> cleanup_timeout_and_collect(int timeout_sec);
    std::vector<Peer> snapshot();
    void clear();

private:
    mutable std::mutex mu_;
    std::map<std::string, Peer> peers_;
    // uuid -> 鉴权有效期：与在线表分离，登录成功即记，不依赖注册
    std::map<std::string, time_t> auth_sessions_;
};

} // namespace p2p

#endif // P2P_PEER_MANAGE_H
