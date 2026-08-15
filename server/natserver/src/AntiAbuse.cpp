#include "AntiAbuse.h"
#include "Log.h"
#include "Util.h"
#include "common/Crypto.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace p2p {

namespace {

constexpr char BL_MAGIC[4] = {'P', '2', 'P', 'B'};
constexpr char BL_PLAIN_HEADER[] = "#P2P-BLACKLIST-v1\n";
constexpr uint32_t PBKDF2_ITER = 10000;
constexpr int SALT_LEN = 16;
constexpr int IV_LEN = 16;

// 明文格式：头行 + "IP <点分> <过期epoch>" / "UUID <uuid> <过期epoch>"
void parse_entries(std::unordered_map<uint32_t, time_t>& ipb,
                   std::unordered_map<std::string, time_t>& uu, const std::string& content) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("IP ", 0) == 0) {
            size_t sp1 = line.find(' ', 3);
            if (sp1 != std::string::npos) {
                uint32_t ip;
                if (inet_pton(AF_INET, line.substr(3, sp1 - 3).c_str(), &ip) == 1) {
                    ipb[ip] = (time_t)atoll(line.substr(sp1 + 1).c_str());
                }
            }
        } else if (line.rfind("UUID ", 0) == 0) {
            size_t sp1 = line.find(' ', 5);
            if (sp1 != std::string::npos) {
                uu[line.substr(5, sp1 - 5)] = (time_t)atoll(line.substr(sp1 + 1).c_str());
            }
        }
    }
}

} // namespace

AntiAbuse::AntiAbuse() {}

void AntiAbuse::configure(uint32_t flood_threshold, uint32_t blacklist_sec) {
    flood_threshold_ = flood_threshold;
    blacklist_sec_ = blacklist_sec;
}

void AntiAbuse::set_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
}

void AntiAbuse::set_password(const std::string& pass) {
    std::lock_guard<std::mutex> lk(mu_);
    pass_ = pass;
}

bool AntiAbuse::needs_save() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dirty_;
}

void AntiAbuse::reset_dirty() {
    std::lock_guard<std::mutex> lk(mu_);
    dirty_ = false;
}

bool AntiAbuse::load() {
    FILE* fp = fopen(path_.c_str(), "rb");
    if (!fp) return false;
    std::string raw;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) raw.append(chunk, n);
    fclose(fp);
    if (raw.empty()) return false;

    std::lock_guard<std::mutex> lk(mu_);
    std::string plain;
    if (raw.size() >= 4 && memcmp(raw.data(), BL_MAGIC, 4) == 0) {
        // 加密格式：magic(4) ver(1) iter(4BE) salt(16) iv(16) ciphertext
        if (raw.size() < 4 + 1 + 4 + SALT_LEN + IV_LEN + 16) return false;
        if (pass_.empty()) {
            LOGE("AntiAbuse", "%s encrypted but BlacklistPass not set", path_.c_str());
            return false;
        }
        size_t off = 5;
        uint32_t iter = ((uint32_t)(uint8_t)raw[off] << 24) |
                        ((uint32_t)(uint8_t)raw[off + 1] << 16) |
                        ((uint32_t)(uint8_t)raw[off + 2] << 8) | (uint8_t)raw[off + 3];
        off += 4;
        const uint8_t* salt = (const uint8_t*)raw.data() + off; off += SALT_LEN;
        const uint8_t* iv = (const uint8_t*)raw.data() + off;   off += IV_LEN;
        const uint8_t* cipher = (const uint8_t*)raw.data() + off;
        size_t clen = raw.size() - off;

        uint8_t key[32];
        if (!pbkdf2_hmac_sha256((const uint8_t*)pass_.data(), pass_.size(),
                                salt, SALT_LEN, iter ? iter : PBKDF2_ITER, key, 32)) {
            LOGE("AntiAbuse", "pbkdf2 derive key failed (bad salt length) for %s", path_.c_str());
            return false;
        }
        plain.resize(clen + 1);
        size_t plen = 0;
        if (aes256_cbc_decrypt(key, iv, cipher, clen, (uint8_t*)plain.data(),
                               plain.size(), &plen) != 0 ||
            plen < sizeof(BL_PLAIN_HEADER) - 1 ||
            memcmp(plain.data(), BL_PLAIN_HEADER, sizeof(BL_PLAIN_HEADER) - 1) != 0) {
            LOGE("AntiAbuse", "decrypt blacklist %s failed (wrong password?)", path_.c_str());
            return false;
        }
        parse_entries(ip_black_, uuid_black_,
                      plain.substr(sizeof(BL_PLAIN_HEADER) - 1, plen));
    } else {
        parse_entries(ip_black_, uuid_black_, raw);
    }
    dirty_ = true;   // 落盘时统一重写为标准格式
    LOGI("AntiAbuse", "loaded blacklist %s: %zu ip %zu uuid", path_.c_str(),
         ip_black_.size(), uuid_black_.size());
    return true;
}

bool AntiAbuse::save() const {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (path_.empty() || !dirty_) return false;
        path = path_;
    }

    // 明文内容（含过期时间，重启后继续生效）
    std::string plain = BL_PLAIN_HEADER;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : ip_black_) {
            if (kv.second <= time(nullptr)) continue;
            char line[64];
            snprintf(line, sizeof(line), "IP %s %lld\n",
                     ip_to_str(kv.first).c_str(), (long long)kv.second);
            plain += line;
        }
        for (auto& kv : uuid_black_) {
            if (kv.second <= time(nullptr)) continue;
            plain += "UUID " + kv.first + " " +
                     std::to_string((long long)kv.second) + "\n";
        }
    }

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { LOGW("AntiAbuse", "open %s for write failed", path.c_str()); return false; }

    if (pass_.empty()) {
        fwrite(plain.data(), 1, plain.size(), fp);
        fclose(fp);
        return true;
    }

    uint8_t salt[SALT_LEN], iv[IV_LEN];
    p2p_random_bytes(salt, SALT_LEN);
    p2p_random_bytes(iv, IV_LEN);
    uint8_t key[32];
    if (!pbkdf2_hmac_sha256((const uint8_t*)pass_.data(), pass_.size(),
                            salt, SALT_LEN, PBKDF2_ITER, key, 32)) {
        fclose(fp);
        LOGE("AntiAbuse", "pbkdf2 derive key failed (bad salt length)");
        return false;
    }
    std::vector<uint8_t> cipher(plain.size() + 16);
    size_t clen = 0;
    if (aes256_cbc_encrypt(key, iv, (const uint8_t*)plain.data(), plain.size(),
                           cipher.data(), cipher.size(), &clen) != 0) {
        fclose(fp);
        LOGE("AntiAbuse", "aes256 encrypt blacklist failed");
        return false;
    }
    uint8_t head[4 + 1 + 4];
    memcpy(head, BL_MAGIC, 4);
    head[4] = 1;
    head[5] = (uint8_t)(PBKDF2_ITER >> 24);
    head[6] = (uint8_t)(PBKDF2_ITER >> 16);
    head[7] = (uint8_t)(PBKDF2_ITER >> 8);
    head[8] = (uint8_t)PBKDF2_ITER;
    fwrite(head, 1, sizeof(head), fp);
    fwrite(salt, 1, SALT_LEN, fp);
    fwrite(iv, 1, IV_LEN, fp);
    fwrite(cipher.data(), 1, clen, fp);
    fclose(fp);
    return true;
}

bool AntiAbuse::check_flood(const sockaddr_in& from) {
    uint32_t ip = from.sin_addr.s_addr;
    time_t now = time(nullptr);

    std::lock_guard<std::mutex> lk(mu_);
    if (ip_black_.find(ip) != ip_black_.end()) return true;

    auto& dq = flood_[ip];
    while (!dq.empty() && now - dq.front() >= 1) dq.pop_front();
    dq.push_back(now);

    if (dq.size() > flood_threshold_) {
        ip_black_[ip] = now + blacklist_sec_;
        dq.clear();
        dirty_ = true;
        dropped_flood_ += flood_threshold_;
        LOGW("AntiAbuse", "flood from ip=%s, blacklisted %lus",
             ip_to_str(ip).c_str(), (unsigned long)blacklist_sec_);
        return true;
    }
    return false;
}

bool AntiAbuse::is_ip_blacklisted(const sockaddr_in& from) {
    return is_ip_blacklisted_ip(from.sin_addr.s_addr);
}

bool AntiAbuse::is_ip_blacklisted_ip(uint32_t ip) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = ip_black_.find(ip);
    if (it == ip_black_.end()) return false;
    if (time(nullptr) > it->second) { ip_black_.erase(it); dirty_ = true; return false; }
    return true;
}

bool AntiAbuse::is_uuid_blacklisted(const std::string& uuid) {
    if (uuid.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uuid_black_.find(uuid);
    if (it == uuid_black_.end()) return false;
    if (time(nullptr) > it->second) { uuid_black_.erase(it); dirty_ = true; return false; }
    return true;
}

void AntiAbuse::blacklist_ip(uint32_t ip) {
    std::lock_guard<std::mutex> lk(mu_);
    ip_black_[ip] = time(nullptr) + blacklist_sec_;
    dirty_ = true;
}

void AntiAbuse::blacklist_uuid(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    uuid_black_[uuid] = time(nullptr) + blacklist_sec_;
    dirty_ = true;
}

void AntiAbuse::unblacklist_ip(uint32_t ip) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ip_black_.erase(ip) > 0) dirty_ = true;
}

void AntiAbuse::unblacklist_uuid(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (uuid_black_.erase(uuid) > 0) dirty_ = true;
}

size_t AntiAbuse::ip_blacklist_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ip_black_.size();
}

size_t AntiAbuse::uuid_blacklist_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return uuid_black_.size();
}

std::map<std::string, time_t> AntiAbuse::ip_blacklist_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, time_t> out;
    char buf[INET_ADDRSTRLEN];
    for (auto& kv : ip_black_) {
        inet_ntop(AF_INET, &kv.first, buf, sizeof(buf));
        out[buf] = kv.second;
    }
    return out;
}

std::map<std::string, time_t> AntiAbuse::uuid_blacklist_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, time_t> out;
    for (auto& kv : uuid_black_) out[kv.first] = kv.second;
    return out;
}

void AntiAbuse::sweep() {
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = ip_black_.begin(); it != ip_black_.end();)
        if (now > it->second) { it = ip_black_.erase(it); dirty_ = true; } else ++it;
    for (auto it = uuid_black_.begin(); it != uuid_black_.end();)
        if (now > it->second) { it = uuid_black_.erase(it); dirty_ = true; } else ++it;
}

} // namespace p2p
