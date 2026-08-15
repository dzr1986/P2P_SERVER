#ifndef P2P_ICE_SDP_H
#define P2P_ICE_SDP_H

// ICE SDP 辅助：从 local/remote description 取 ice-ufrag。
// libjuice 不支持在同一 agent 上换 ufrag，ICE restart = 重建 agent。
// 对端 ufrag 变化即视为 restart（RFC 8445）。

#include <cstring>
#include <string>

namespace p2p {

inline std::string ice_sdp_attr(const std::string& sdp, const char* key) {
    if (!key || !*key) return {};
    const size_t klen = std::strlen(key);
    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t line = sdp.find(key, pos);
        if (line == std::string::npos) return {};
        const bool at_bol = (line == 0 || sdp[line - 1] == '\n' || sdp[line - 1] == '\r');
        if (!at_bol) { pos = line + 1; continue; }
        size_t val = line + klen;
        size_t end = sdp.find_first_of("\r\n", val);
        if (end == std::string::npos) end = sdp.size();
        return sdp.substr(val, end - val);
    }
    return {};
}

inline std::string ice_sdp_ufrag(const std::string& sdp) {
    return ice_sdp_attr(sdp, "a=ice-ufrag:");
}

inline std::string ice_sdp_pwd(const std::string& sdp) {
    return ice_sdp_attr(sdp, "a=ice-pwd:");
}

// 两端都有 ufrag 且不同 → ICE restart（换一代凭证）
inline bool ice_sdp_is_restart(const std::string& old_sdp, const std::string& new_sdp) {
    if (old_sdp.empty() || new_sdp.empty()) return false;
    const std::string a = ice_sdp_ufrag(old_sdp);
    const std::string b = ice_sdp_ufrag(new_sdp);
    return !a.empty() && !b.empty() && a != b;
}

} // namespace p2p

#endif // P2P_ICE_SDP_H
