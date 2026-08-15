#ifndef P2P_LICENSE_MGR_H
#define P2P_LICENSE_MGR_H

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace p2p {

// ---------------------------------------------------------------------------
// 授权管理（对标原版 LicenseManager）
// 白名单文件每行一个 UUID；空表 = 不限制
// 设置密码后持久化为 PBKDF2+AES-256-CBC 加密文件（无密码=明文兼容旧格式）
// ---------------------------------------------------------------------------
class LicenseMgr {
public:
    LicenseMgr();
    ~LicenseMgr();

    bool load(const std::string& path);            // 从文件加载（追加）
    bool save() const;                              // 写回文件（加密或明文）
    bool set_path(const std::string& path);
    void set_password(const std::string& pass);     // 空=明文存储

    bool add(const std::string& uuid);
    bool remove(const std::string& uuid);
    bool exists(const std::string& uuid) const;
    size_t count() const;
    std::vector<std::string> list() const;

    // 注册门控：白名单为空时放行全部
    bool allowed(const std::string& uuid) const;

private:
    std::string path_;
    std::string pass_;
    std::set<std::string> uuids_;
    mutable std::mutex mu_;
};

} // namespace p2p

#endif // P2P_LICENSE_MGR_H
