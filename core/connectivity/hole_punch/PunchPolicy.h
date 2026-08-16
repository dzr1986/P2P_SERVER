#ifndef P2P_CORE_HOLE_PUNCH_PUNCH_POLICY_H
#define P2P_CORE_HOLE_PUNCH_PUNCH_POLICY_H

// 对齐 EasyTier hole_punch/policy.rs：序列 BackOff + 是否打洞 / 是否后台打洞。
// 只吃事实结构体，不持锁、不调 juice。
// force_relay 比 EasyTier 的 disable_p2p 更硬：对端 need_p2p 也不能打（测试 [2]/[11]）。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace p2p {

class PunchBackOff {
public:
    PunchBackOff() : PunchBackOff(hole_punch_steps()) {}
    explicit PunchBackOff(std::vector<uint32_t> steps)
        : steps_(steps.empty() ? hole_punch_steps() : std::move(steps)) {}

    static std::vector<uint32_t> hole_punch_steps() {
        return {1000, 1000, 2000, 4000, 4000, 8000, 8000, 16000};
    }

    static PunchBackOff hole_punch() { return PunchBackOff(hole_punch_steps()); }

    static PunchBackOff exp(uint32_t base_ms, uint32_t cap_ms, int n = 8) {
        if (base_ms == 0) base_ms = 1;
        if (cap_ms < base_ms) cap_ms = base_ms;
        std::vector<uint32_t> s;
        uint32_t d = base_ms;
        for (int i = 0; i < n; i++) {
            s.push_back(d);
            if (d >= cap_ms) break;
            const uint32_t next = d > cap_ms / 2 ? cap_ms : d * 2;
            d = next > cap_ms ? cap_ms : next;
        }
        if (s.back() < cap_ms) s.push_back(cap_ms);
        return PunchBackOff(std::move(s));
    }

    uint32_t next() {
        const uint32_t v = steps_[idx_];
        if (idx_ + 1 < steps_.size()) idx_++;
        return v;
    }

    void rollback() {
        if (idx_ > 0) idx_--;
    }

    void reset() { idx_ = 0; }
    size_t index() const { return idx_; }

private:
    std::vector<uint32_t> steps_;
    size_t idx_ = 0;
};

// EasyTier PeerFeatureFlag 的打洞相关位（本仓库没有 mesh 对等体标志，测试与适配用）。
struct PunchPeerFlag {
    bool need_p2p = false;
    bool disable_p2p = false;
    bool is_public_server = false;
};

inline bool should_try_p2p_with_peer(const PunchPeerFlag* flag,
                                     bool allow_public_server,
                                     bool local_disable_p2p,
                                     bool local_need_p2p) {
    if (!flag) return !local_disable_p2p;
    return (allow_public_server || !flag->is_public_server) &&
           (!local_disable_p2p || flag->need_p2p) &&
           (!flag->disable_p2p || local_need_p2p);
}

inline bool should_background_p2p_with_peer(const PunchPeerFlag* flag,
                                            bool allow_public_server,
                                            bool lazy_p2p,
                                            bool local_disable_p2p,
                                            bool local_need_p2p) {
    if (!should_try_p2p_with_peer(flag, allow_public_server,
                                  local_disable_p2p, local_need_p2p))
        return false;
    if (!lazy_p2p) return true;
    return flag && flag->need_p2p;
}

// IoT 门：现有 lazy_p2p / want_direct / force_relay。
struct PunchGate {
    bool force_relay = false;
    bool lazy_p2p = false;
    bool want_direct = false;     // 本端已有业务发送
    bool peer_need_p2p = false;   // 对端在打洞（如 TCP announce）
};

inline bool should_try_p2p(const PunchGate& g) {
    if (g.force_relay) return false;
    PunchPeerFlag f;
    f.need_p2p = g.peer_need_p2p;
    return should_try_p2p_with_peer(&f, true, false, g.want_direct);
}

// 中继已通后的后台打洞：lazy 关则打；lazy 开则本端有业务或对端在打才打。
inline bool should_background_p2p(const PunchGate& g) {
    if (g.force_relay) return false;
    PunchPeerFlag f;
    f.need_p2p = g.want_direct || g.peer_need_p2p;
    return should_background_p2p_with_peer(&f, true, g.lazy_p2p, false, g.want_direct);
}

}  // namespace p2p

#endif
