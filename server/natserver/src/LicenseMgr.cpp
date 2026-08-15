#include "LicenseMgr.h"
#include "Log.h"
#include "common/Crypto.h"

#include <cstdio>
#include <cstring>

namespace p2p {

namespace {

constexpr char ENC_MAGIC[4] = {'P', '2', 'P', 'L'};
constexpr char PLAIN_HEADER[] = "#P2P-LICENSE-v1\n";
constexpr uint32_t PBKDF2_ITER = 10000;
constexpr int SALT_LEN = 16;
constexpr int IV_LEN = 16;

// 每行解析一个 uuid（# 开头为注释）
bool parse_uuids(std::set<std::string>& out, const std::string& content) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (!line.empty()) out.insert(line);
    }
    return true;
}

} // namespace

LicenseMgr::LicenseMgr() {}
LicenseMgr::~LicenseMgr() {}

bool LicenseMgr::set_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    return true;
}

void LicenseMgr::set_password(const std::string& pass) {
    std::lock_guard<std::mutex> lk(mu_);
    pass_ = pass;
}

bool LicenseMgr::load(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        path_ = path;

        // 读入整个文件
        std::string raw;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) raw.append(chunk, n);
        fclose(fp);

        if (raw.size() >= 4 && memcmp(raw.data(), ENC_MAGIC, 4) == 0) {
            // ---- 加密格式：magic(4) ver(1) iter(4BE) salt(16) iv(16) ciphertext ----
            if (raw.size() < 4 + 1 + 4 + SALT_LEN + IV_LEN + 16) {
                LOGE("LicenseMgr", "encrypted license file %s too short", path.c_str());
                return false;
            }
            if (pass_.empty()) {
                LOGE("LicenseMgr", "%s is encrypted but LicensePass not set", path.c_str());
                return false;
            }
            size_t off = 5;  // magic + ver
            uint32_t iter = ((uint32_t)(uint8_t)raw[off] << 24) |
                            ((uint32_t)(uint8_t)raw[off + 1] << 16) |
                            ((uint32_t)(uint8_t)raw[off + 2] << 8) |
                            (uint8_t)raw[off + 3];
            off += 4;
            const uint8_t* salt = (const uint8_t*)raw.data() + off;
            off += SALT_LEN;
            const uint8_t* iv = (const uint8_t*)raw.data() + off;
            off += IV_LEN;
            const uint8_t* cipher = (const uint8_t*)raw.data() + off;
            size_t clen = raw.size() - off;

            uint8_t key[32];
            if (!pbkdf2_hmac_sha256((const uint8_t*)pass_.data(), pass_.size(),
                                    salt, SALT_LEN, iter ? iter : PBKDF2_ITER, key, 32)) {
                LOGE("LicenseMgr", "pbkdf2 derive key failed (bad salt length) for %s", path.c_str());
                return false;
            }
            std::string plain;
            plain.resize(clen + 1);
            size_t plen = 0;
            if (aes256_cbc_decrypt(key, iv, cipher, clen,
                                   (uint8_t*)plain.data(), plain.size(), &plen) != 0 ||
                plen < sizeof(PLAIN_HEADER) - 1 ||
                memcmp(plain.data(), PLAIN_HEADER, sizeof(PLAIN_HEADER) - 1) != 0) {
                LOGE("LicenseMgr", "decrypt %s failed (wrong password/salt?)", path.c_str());
                return false;
            }
            uuids_.clear();
            parse_uuids(uuids_, plain.substr(sizeof(PLAIN_HEADER) - 1, plen));
            LOGI("LicenseMgr", "loaded encrypted %s: %zu uuids", path.c_str(), count());
            return true;
        }

        // ---- 明文格式（兼容旧文件）：每行一个 uuid ----
        uuids_.clear();
        parse_uuids(uuids_, raw);
        LOGI("LicenseMgr", "loaded %s: %zu uuids", path.c_str(), count());
    }
    return true;
}

bool LicenseMgr::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (path_.empty()) return false;
    FILE* fp = fopen(path_.c_str(), "wb");
    if (!fp) return false;

    // 明文内容：头行 + 每行一个 uuid
    std::string plain = PLAIN_HEADER;
    for (const auto& u : uuids_) plain += u + "\n";

    if (pass_.empty()) {
        // 明文写回
        fwrite(plain.data(), 1, plain.size(), fp);
        fclose(fp);
        return true;
    }

    // 加密写回
    uint8_t salt[SALT_LEN], iv[IV_LEN];
    p2p_random_bytes(salt, SALT_LEN);
    p2p_random_bytes(iv, IV_LEN);
    uint8_t key[32];
    if (!pbkdf2_hmac_sha256((const uint8_t*)pass_.data(), pass_.size(),
                            salt, SALT_LEN, PBKDF2_ITER, key, 32)) {
        fclose(fp);
        LOGE("LicenseMgr", "pbkdf2 derive key failed (bad salt length)");
        return false;
    }

    std::vector<uint8_t> cipher(plain.size() + 16);
    size_t clen = 0;
    if (aes256_cbc_encrypt(key, iv, (const uint8_t*)plain.data(), plain.size(),
                           cipher.data(), cipher.size(), &clen) != 0) {
        fclose(fp);
        LOGE("LicenseMgr", "aes256 encrypt failed");
        return false;
    }

    uint8_t head[4 + 1 + 4];
    memcpy(head, ENC_MAGIC, 4);
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

bool LicenseMgr::add(const std::string& uuid) {
    if (uuid.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto r = uuids_.insert(uuid);
    return r.second;
}

bool LicenseMgr::remove(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    return uuids_.erase(uuid) > 0;
}

bool LicenseMgr::exists(const std::string& uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    return uuids_.find(uuid) != uuids_.end();
}

size_t LicenseMgr::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return uuids_.size();
}

std::vector<std::string> LicenseMgr::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    v.reserve(uuids_.size());
    for (const auto& u : uuids_) v.push_back(u);
    return v;
}

bool LicenseMgr::allowed(const std::string& uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (uuids_.empty()) return true;
    return uuids_.find(uuid) != uuids_.end();
}

} // namespace p2p
