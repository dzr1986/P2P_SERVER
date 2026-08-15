#ifndef P2P_ANTI_ABUSE_H
#define P2P_ANTI_ABUSE_H

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

// ---------------------------------------------------------------------------
// 防滥用：滑动窗口防洪水 + IP/UUID 黑名单（对标原版 do_flood_ip_drop）
// ---------------------------------------------------------------------------
class AntiAbuse {
public:
    AntiAbuse();

    void configure(uint32_t flood_threshold, uint32_t blacklist_sec);

    // 黑名单持久化（重启不丢；带口令则 PBKDF2+AES-256 加密落盘）
    void set_path(const std::string& path);
    void set_password(const std::string& pass);
    bool load();                     // 启动时加载，失败不影响运行
    bool save() const;               // 有变更时写回
    bool needs_save() const;         // 是否有未落盘变更
    void reset_dirty();              // 落盘成功后清变更标记

    // 检测洪水：1s 窗口内超过阈值则拉黑该 IP，返回 true
    bool check_flood(const sockaddr_in& from);

    // 黑名单判断（命中自动过期清理）
    bool is_ip_blacklisted(const sockaddr_in& from);
    bool is_ip_blacklisted_ip(uint32_t ip);
    bool is_uuid_blacklisted(const std::string& uuid);

    void blacklist_ip(uint32_t ip);
    void blacklist_uuid(const std::string& uuid);
    void unblacklist_ip(uint32_t ip);
    void unblacklist_uuid(const std::string& uuid);

    size_t ip_blacklist_size() const;
    size_t uuid_blacklist_size() const;
    // 快照：返回 (key -> 过期 epoch)，供管理接口查询
    std::map<std::string, time_t> ip_blacklist_snapshot() const;
    std::map<std::string, time_t> uuid_blacklist_snapshot() const;

    uint64_t dropped_flood_pkts() const { return dropped_flood_; }

    // 周期性清理过期黑名单
    void sweep();

private:
    uint32_t flood_threshold_ = 200;
    uint32_t blacklist_sec_ = 60;

    // ip -> 1s 窗口内的到达时间戳（滑动窗口）
    std::unordered_map<uint32_t, std::deque<time_t>> flood_;
    // ip/uuid -> 拉黑截止时间
    std::unordered_map<uint32_t, time_t> ip_black_;
    std::unordered_map<std::string, time_t> uuid_black_;

    // 持久化状态
    std::string path_;
    std::string pass_;
    mutable bool dirty_ = false;

    std::atomic<uint64_t> dropped_flood_{0};

    mutable std::mutex mu_;
};

} // namespace p2p

#endif // P2P_ANTI_ABUSE_H
