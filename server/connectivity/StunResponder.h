#ifndef P2P_SERVER_CONNECTIVITY_STUN_RESPONDER_H
#define P2P_SERVER_CONNECTIVITY_STUN_RESPONDER_H

// 对齐 EasyTier connectivity/stun/responder.rs：
//   解析 Binding，CHANGE-REQUEST 则从备用口回（SameSocket / OtherSocket）。
// 只编排，不持锁、不碰业务表。TCP STUN 仍走同口 32 字节 Success。

#include "core/packet/StunBind.h"

#include <cstdint>

namespace p2p {

enum class StunSendSrc : uint8_t { Same = 0, Other = 1 };

struct StunReplyPlan {
    uint8_t     buf[64]{};
    size_t      len = 0;
    StunSendSrc src = StunSendSrc::Same;
};

inline StunReplyPlan stun_plan_reply(const uint8_t* req, size_t len,
                                     const sockaddr_in& mapped,
                                     const sockaddr_in* other_addr,
                                     bool have_other_socket) {
    StunReplyPlan p;
    bool change_ip = false, change_port = false;
    stun_parse_change_request(req, len, &change_ip, &change_port);
    if ((change_ip || change_port) && have_other_socket)
        p.src = StunSendSrc::Other;
    p.len = stun_write_binding_success_ex(p.buf, sizeof(p.buf), req, len,
                                          mapped, other_addr);
    return p;
}

inline bool stun_respond(int same_fd, int other_fd,
                         const uint8_t* req, size_t len,
                         const sockaddr_in& from,
                         const sockaddr_in* other_addr) {
    const bool have_other = other_fd >= 0;
    const StunReplyPlan p = stun_plan_reply(req, len, from, other_addr, have_other);
    if (p.len == 0 || same_fd < 0) return false;
    const int fd = (p.src == StunSendSrc::Other) ? other_fd : same_fd;
    if (fd < 0) return false;
    sendto(fd, p.buf, p.len, 0,
           reinterpret_cast<const sockaddr*>(&from), sizeof(from));
    return true;
}

}  // namespace p2p

#endif
