// RecvProcess.cpp：NatServer 报文解析与分发（仿原实现 RecvProcess.cpp）
#include "NatServer.h"
#include "Crypto.h"
#include "Log.h"
#include "Util.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

namespace p2p {

static void copy_uuid(char* dst, const char* src) {
    memcpy(dst, src, MAX_UUID_LEN);
    dst[MAX_UUID_LEN] = 0;
}

void NatServer::handle_packet(const uint8_t* data, size_t len, const sockaddr_in& from) {
    if (len < sizeof(MsgHead)) return;

    MsgHead h;
    memcpy(&h, data, sizeof(h));
    if (ntohs(h.magic) != NAT_MAGIC) return;
    if (h.version != PROTO_VER) return;
    uint32_t hdr_len = ntohl(h.length);
    if (hdr_len > len - sizeof(MsgHead)) return;

    const uint8_t* p = data + sizeof(MsgHead);
    size_t plen = hdr_len;

    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
    uint16_t from_port = ntohs(from.sin_port);
    auto cfg = cfg_;

    switch (h.msg_id) {
    case MSG_NAT_DETECT_REQ:
        // 已被收包线程快速路径处理，理论上不会到这里；防御性忽略
        break;

    // ---------------------------------------------------------------- 心跳/注册
    case MSG_HEARTBEAT_REQ:
    case MSG_ADD_UID_REQ: {
        if (plen < sizeof(UuidReq)) break;
        UuidReq req;
        memcpy(&req, p, sizeof(UuidReq));
        req.uuid[MAX_UUID_LEN] = 0;

        // 扩展信息（长度受控）
        uint16_t extlen = ntohs(req.extlen);
        std::string extinfo;
        if (extlen > 0 && extlen <= MAX_EXTINFO && plen >= sizeof(UuidReq) + extlen) {
            extinfo.assign((const char*)p + sizeof(UuidReq), extlen);
        }
        handle_heartbeat(req, extinfo, from, false, nullptr);
        break;
    }

    case MSG_HEARTBEAT_REQ_ENC: {
        // 负载 = uuid(33B 明文) || iv(8) || cipher(UuidReq 完整定长 + 扩展信息)
        if (plen < 33 + 8 + sizeof(UuidReq)) break;
        char uuid[MAX_UUID_LEN + 1];
        memcpy(uuid, p, 33);
        uuid[MAX_UUID_LEN] = 0;
        const uint8_t* iv = p + 33;
        size_t cipher_len = plen - (33 + 8);

        uint8_t body[MAX_PKT];
        memcpy(body, p + 33 + 8, cipher_len);
        if (!cfg->auth_secret.empty()) {
            p2p_stream_xor((const uint8_t*)cfg->auth_secret.data(),
                           cfg->auth_secret.size(), uuid, iv, body, cipher_len);
        }
        // 还原完整 UuidReq（解密后整段即 UuidReq；外层明文 uuid 覆盖防路由篡改）
        UuidReq req;
        memset(&req, 0, sizeof(req));
        memcpy(&req, body, sizeof(UuidReq));
        memcpy(req.uuid, uuid, MAX_UUID_LEN);
        req.uuid[MAX_UUID_LEN] = 0;

        std::string extinfo;
        if (cipher_len > sizeof(UuidReq)) {
            size_t ext = cipher_len - sizeof(UuidReq);
            if (ext <= MAX_EXTINFO)
                extinfo.assign((char*)body + sizeof(UuidReq), ext);
        }
        handle_heartbeat(req, extinfo, from, true, iv);
        break;
    }

    // ---------------------------------------------------------------- 地址/扩展信息
    case MSG_SND_EXTINFO_REQ:
    case MSG_ASK_EXTINFO_REQ: {
        // payload: ExtInfoReq(dst uuid) [+ 上报方扩展信息（SND 时）]
        ExtInfoRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        inet_ntop(AF_INET, &from.sin_addr, rsp.pub_ip, MAX_IP_LEN);
        rsp.pub_port = from.sin_port;
        rsp.nat_sock2_port = htons(natcheck_.alt_port());

        if (plen >= sizeof(ExtInfoReq)) {
            ExtInfoReq req;
            memcpy(&req, p, sizeof(ExtInfoReq));
            req.uuid[MAX_UUID_LEN] = 0;

            if (h.msg_id == MSG_SND_EXTINFO_REQ) {
                // 上报：更新本机扩展信息，若目标在线则转发
                uint16_t extlen = (uint16_t)(plen - sizeof(ExtInfoReq));
                std::string extinfo;
                if (extlen <= MAX_EXTINFO)
                    extinfo.assign((const char*)p + sizeof(ExtInfoReq), extlen);
                Peer self;
                if (peers_.get(req.uuid, self)) {
                    peers_.upsert(self.uuid, self.pub_addr, self.lan_addr,
                                  self.dev_type, self.nattype, extinfo);
                }
                Peer dst;
                if (peers_.get(req.uuid, dst)) {
                    send_msg(dst.pub_addr, MSG_SND_EXTINFO_REQ, p, plen);
                }
                break;
            }

            Peer pe;
            if (peers_.get(req.uuid, pe)) {
                inet_ntop(AF_INET, &pe.lan_addr.sin_addr, rsp.lan_ip, MAX_IP_LEN);
                rsp.lan_port = pe.lan_addr.sin_port;
                rsp.result = 0;
                // 目标扩展信息拼在定长应答之后
                uint8_t buf[MAX_PKT];
                memset(buf, 0, sizeof(buf));
                memcpy(buf, &rsp, sizeof(rsp));
                size_t off = sizeof(rsp);
                if (!pe.extinfo.empty() && off + pe.extinfo.size() <= sizeof(buf)) {
                    memcpy(buf + off, pe.extinfo.data(), pe.extinfo.size());
                    off += pe.extinfo.size();
                }
                send_msg(from, MSG_ASK_EXTINFO_RSP, buf, off);
            } else {
                rsp.result = 2;   // 目标不在线
                send_msg(from, MSG_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
            }
        } else {
            send_msg(from, MSG_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
        }
        break;
    }

    // ---------------------------------------------------------------- CONNECT 协调
    case MSG_CONNECT_REQ: {
        if (plen < sizeof(ConnectReq)) break;
        ConnectReq req;
        memcpy(&req, p, sizeof(ConnectReq));
        req.src_uuid[MAX_UUID_LEN] = 0;
        req.dst_uuid[MAX_UUID_LEN] = 0;

        LOGI("NatServer", "CONNECT_REQ src[%s] dst[%s] from [%s:%d]",
             req.src_uuid, req.dst_uuid, ipbuf, from_port);

        ConnectAck ack;
        memset(&ack, 0, sizeof(ack));
        copy_uuid(ack.dst_uuid, req.dst_uuid);

        // 鉴权门
        if (cfg->auth_enabled() && !peers_.authed(req.src_uuid)) {
            ack.result = 3;   // 需鉴权
            send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
            inc_connect_fail();
            break;
        }
        if (cfg->enable_license && !license_.allowed(req.src_uuid)) {
            ack.result = CONNECT_NOT_FOUND;
            send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
            inc_connect_fail();
            break;
        }

        Peer dst;
        if (!peers_.get(req.dst_uuid, dst)) {
            ack.result = CONNECT_NOT_FOUND;
            send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
            LOGI("NatServer", "ack===>dst UUID[%s] not found", req.dst_uuid);
            inc_connect_fail();
            break;
        }

        // ---- 应答发起方 ----
        ack.result = CONNECT_OK;
        inet_ntop(AF_INET, &dst.pub_addr.sin_addr, ack.dst_pub_ip, MAX_IP_LEN);
        ack.dst_pub_port = dst.pub_addr.sin_port;
        inet_ntop(AF_INET, &dst.lan_addr.sin_addr, ack.dst_lan_ip, MAX_IP_LEN);
        ack.dst_lan_port = dst.lan_addr.sin_port;
        ack.dst_nattype = dst.nattype;
        pick_proxy(ack.proxies, ack.proxy_count);
        send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
        LOGI("NatServer", "ack===>to initiator [%s], dst pub[%s:%d] nattype[%d] proxy[%d]",
             req.src_uuid, ack.dst_pub_ip, ntohs(ack.dst_pub_port), dst.nattype,
             ack.proxy_count);

        // ---- 通知目标方 ----
        ConnectInvite inv;
        memset(&inv, 0, sizeof(inv));
        copy_uuid(inv.src_uuid, req.src_uuid);
        inet_ntop(AF_INET, &from.sin_addr, inv.src_pub_ip, MAX_IP_LEN);
        inv.src_pub_port = from.sin_port;

        Peer src;
        if (peers_.get(req.src_uuid, src)) {
            inet_ntop(AF_INET, &src.lan_addr.sin_addr, inv.src_lan_ip, MAX_IP_LEN);
            inv.src_lan_port = src.lan_addr.sin_port;
            inv.src_nattype = src.nattype;
        } else {
            inet_ntop(AF_INET, &from.sin_addr, inv.src_lan_ip, MAX_IP_LEN);
            inv.src_lan_port = from.sin_port;
            inv.src_nattype = NAT_UNKNOWN;
        }
        pick_proxy(inv.proxies, inv.proxy_count);
        send_msg(dst.pub_addr, MSG_CONNECT_INVITE, &inv, sizeof(inv));
        LOGI("NatServer", "invite===>to dst UUID[%s] port:[%d] nattype[%d]",
             req.dst_uuid, ntohs(dst.pub_addr.sin_port), inv.src_nattype);

        inc_connect_ok();
        break;
    }

    // ---------------------------------------------------------------- #19 ICE SDP 中转
    case MSG_ICE_SDP: {
        // 变长消息：IceSdpMsg { uuid; sdp_len(net); sdp[] }
        if (plen < (int)(sizeof(IceSdpMsg) - 1)) break;
        IceSdpMsg* m = (IceSdpMsg*)p;
        m->uuid[MAX_UUID_LEN] = 0;
        uint16_t slen = ntohs(m->sdp_len);
        if ((size_t)plen < (sizeof(IceSdpMsg) - 1 + slen)) break;
        Peer dst;
        if (!peers_.get(m->uuid, dst)) {
            LOGW("NatServer", "ICE_SDP dst[%s] not found, drop", m->uuid);
            break;
        }
        send_msg(dst.pub_addr, MSG_ICE_SDP, p, plen);
        LOGI("NatServer", "ICE_SDP===>to dst UUID[%s] len[%d]", m->uuid, (int)plen);
        break;
    }

    // ---------------------------------------------------------------- 设备/服务器列表
    case MSG_GET_DEV_LIST_REQ: {
        DevListReq req;
        memset(&req, 0, sizeof(req));
        if (plen >= sizeof(DevListReq)) memcpy(&req, p, sizeof(DevListReq));
        uint16_t start = ntohs(req.start_index);
        uint16_t want = ntohs(req.want_num);
        if (want == 0 || want > 100) want = 20;

        auto snap = peers_.snapshot();
        std::vector<uint8_t> buf(64 + want * sizeof(DevListEntry), 0);

        DevListRsp* rsp = reinterpret_cast<DevListRsp*>(buf.data());
        rsp->total = htons((uint16_t)snap.size());
        rsp->start_index = htons(start);

        size_t off = sizeof(DevListRsp);
        uint16_t count = 0;
        for (size_t i = start; i < snap.size() && count < want; ++i, ++count) {
            DevListEntry* e = reinterpret_cast<DevListEntry*>(buf.data() + off);
            strncpy(e->uuid, snap[i].uuid.c_str(), MAX_UUID_LEN);
            inet_ntop(AF_INET, &snap[i].pub_addr.sin_addr, e->ip, MAX_IP_LEN);
            e->port = snap[i].pub_addr.sin_port;
            e->dev_type = snap[i].dev_type;
            off += sizeof(DevListEntry);
        }
        rsp->count = htons(count);
        send_msg(from, MSG_GET_DEV_LIST_RSP, buf.data(), off);
        break;
    }

    case MSG_GET_SERVER_LIST_REQ: {
        std::vector<std::string> nats = cfg->nat_ips;
        if (!wan_ip_.empty() && wan_ip_ != "0.0.0.0") {
            bool dup = false;
            for (auto& ip : nats) if (ip == wan_ip_) { dup = true; break; }
            if (!dup) nats.push_back(wan_ip_);
        }
        std::vector<std::string> proxies = cfg->proxy_ips;

        uint8_t buf[128 + 32 * MAX_IP_LEN];
        memset(buf, 0, sizeof(buf));
        ServerListRsp* rsp = reinterpret_cast<ServerListRsp*>(buf);
        rsp->nat_count = htons((uint16_t)nats.size());
        rsp->proxy_count = htons((uint16_t)proxies.size());

        size_t off = sizeof(ServerListRsp);
        for (auto& ip : nats) {
            char* d = reinterpret_cast<char*>(buf + off);
            strncpy(d, ip.c_str(), MAX_IP_LEN - 1);
            off += MAX_IP_LEN;
        }
        for (auto& ip : proxies) {
            char* d = reinterpret_cast<char*>(buf + off);
            strncpy(d, ip.c_str(), MAX_IP_LEN - 1);
            off += MAX_IP_LEN;
        }
        send_msg(from, MSG_GET_SERVER_LIST_RSP, buf, off);
        break;
    }

    // ---------------------------------------------------------------- UID 授权
    case MSG_DELETE_UID_REQ: {
        UuidReq req;
        memset(&req, 0, sizeof(req));
        if (plen >= sizeof(UuidReq)) memcpy(&req, p, sizeof(UuidReq));
        else if (plen >= MAX_UUID_LEN + 1) memcpy(req.uuid, p, MAX_UUID_LEN + 1);
        req.uuid[MAX_UUID_LEN] = 0;
        peers_.remove(req.uuid);
        if (license_.remove(req.uuid)) license_.save();
        LOGI("NatServer", "DELETE_UID UUID=[%s]", req.uuid);
        break;
    }

    case MSG_CHECK_UID_REQ: {
        ExtInfoReq req;
        memset(&req, 0, sizeof(req));
        if (plen >= sizeof(ExtInfoReq)) memcpy(&req, p, sizeof(ExtInfoReq));
        req.uuid[MAX_UUID_LEN] = 0;
        uint8_t result = (peers_.exists(req.uuid) || license_.exists(req.uuid)) ? 1 : 0;
        send_msg(from, MSG_CHECK_UID_RSP, &result, sizeof(result));
        break;
    }

    // ---------------------------------------------------------------- 鉴权
    case MSG_AUTH_CHALLENGE_REQ: {
        AuthChallengeReq req;
        memset(&req, 0, sizeof(req));
        if (plen >= sizeof(AuthChallengeReq)) memcpy(&req, p, sizeof(AuthChallengeReq));
        req.uuid[MAX_UUID_LEN] = 0;

        AuthChallengeRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.nonce_ttl = htons(30);

        if (abuse_.is_uuid_blacklisted(req.uuid)) { rsp.result = 1; }
        else if (cfg->enable_license && !license_.allowed(req.uuid)) { rsp.result = 2; }
        else {
            rsp.result = 0;
            issue_auth_nonce(req.uuid, rsp.nonce);
        }
        send_msg(from, MSG_AUTH_CHALLENGE_RSP, &rsp, sizeof(rsp));
        LOGI("NatServer", "AUTH_CHALLENGE uuid[%s] result[%d]", req.uuid, rsp.result);
        break;
    }

    case MSG_AUTH_LOGIN_REQ: {
        if (plen < sizeof(AuthLoginReq)) break;
        AuthLoginReq req;
        memcpy(&req, p, sizeof(AuthLoginReq));
        req.uuid[MAX_UUID_LEN] = 0;

        AuthLoginRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.session_ttl = htons(3600);
        if (abuse_.is_uuid_blacklisted(req.uuid)) {
            rsp.result = AUTH_BLACKLIST;
        } else if (cfg->enable_license && !license_.allowed(req.uuid)) {
            rsp.result = AUTH_WHITELIST_REJ;
        } else if (verify_auth_login(req.uuid, req.nonce, req.mac)) {
            rsp.result = AUTH_OK;
            LOGI("NatServer", "AUTH_LOGIN uuid[%s] OK", req.uuid);
        } else {
            rsp.result = AUTH_BAD_MAC;
            LOGW("NatServer", "AUTH_LOGIN uuid[%s] FAIL", req.uuid);
        }
        send_msg(from, MSG_AUTH_LOGIN_RSP, &rsp, sizeof(rsp));
        break;
    }

    // ---------------------------------------------------------------- 管理统计
    case MSG_ADMIN_STATS_REQ: {
        AdminStatsRsp rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.total_pkts = htonl((uint32_t)total_pkts_.load());
        rsp.peers_online = htonl((uint32_t)peers_.size());
        rsp.devices = htonl((uint32_t)peers_.device_count());
        rsp.apps = htonl((uint32_t)peers_.client_count());
        rsp.blacklist_ips = htonl((uint32_t)abuse_.ip_blacklist_size());
        rsp.authed_peers = htonl((uint32_t)peers_.authed_count());
        {
            std::lock_guard<std::mutex> lk(proxy_mu_);
            uint32_t ok = 0;
            for (auto& p : proxy_health_) if (p.available) ok++;
            rsp.proxy_ok = htonl(ok);
        }
        rsp.connect_ok = htonl((uint32_t)connect_ok_.load());
        rsp.connect_fail = htonl((uint32_t)connect_fail_.load());
        send_msg(from, MSG_ADMIN_STATS_RSP, &rsp, sizeof(rsp));
        break;
    }

    // ---------------------------------------------------------------- 管理黑名单（AdminSecret 校验）
    case MSG_ADMIN_BLACKLIST_REQ: {
        if (plen < sizeof(AdminBlacklistReq)) break;
        AdminBlacklistReq req;
        memcpy(&req, p, sizeof(AdminBlacklistReq));

        AdminBlacklistRsp rsp;
        memset(&rsp, 0, sizeof(rsp));

        auto sec = cfg->admin_secret;
        if (sec.empty()) {
            rsp.result = 1;   // 未配置 AdminSecret，拒绝
            send_msg(from, MSG_ADMIN_BLACKLIST_RSP, &rsp, sizeof(rsp));
            break;
        }
        // mac = HMAC-SHA256(AdminSecret, op(1B) || key(64B 补零))
        uint8_t mac_in[1 + 64];
        mac_in[0] = req.op;
        memcpy(mac_in + 1, req.key, 64);
        uint8_t expect[32];
        hmac_sha256((const uint8_t*)sec.data(), sec.size(), mac_in, sizeof(mac_in), expect);
        if (memcmp(expect, req.mac, 32) != 0) {
            rsp.result = 1;
            LOGW("NatServer", "admin blacklist bad mac from [%s:%d]", ipbuf, from_port);
            send_msg(from, MSG_ADMIN_BLACKLIST_RSP, &rsp, sizeof(rsp));
            break;
        }

        std::string key(req.key);
        size_t klen = strnlen(req.key, 64);
        key.resize(klen);
        switch (req.op) {
        case 1: {   // 加 IP
            uint32_t ip;
            if (inet_pton(AF_INET, key.c_str(), &ip) != 1) { rsp.result = 4; break; }
            abuse_.blacklist_ip(ip);
            rsp.result = 0;
            break;
        }
        case 2: {   // 删 IP
            uint32_t ip;
            if (inet_pton(AF_INET, key.c_str(), &ip) != 1) { rsp.result = 4; break; }
            if (!abuse_.is_ip_blacklisted_ip(ip)) { rsp.result = 3; break; }
            abuse_.unblacklist_ip(ip);
            rsp.result = 0;
            break;
        }
        case 3: {   // 加 UUID
            if (key.empty() || key.size() > MAX_UUID_LEN) { rsp.result = 4; break; }
            abuse_.blacklist_uuid(key);
            rsp.result = 0;
            break;
        }
        case 4: {   // 删 UUID
            if (key.empty() || key.size() > MAX_UUID_LEN) { rsp.result = 4; break; }
            if (!abuse_.is_uuid_blacklisted(key)) { rsp.result = 3; break; }
            abuse_.unblacklist_uuid(key);
            rsp.result = 0;
            break;
        }
        case 5: {   // 查询（IP 与 UUID 各返回前若干，封顶 28）
            uint8_t n = 0;
            for (auto& kv : abuse_.ip_blacklist_snapshot()) {
                if (n >= 28) break;
                strncpy(rsp.entries[n].key, kv.first.c_str(), 63);
                rsp.entries[n].expire = htonl((uint32_t)kv.second);
                n++;
            }
            for (auto& kv : abuse_.uuid_blacklist_snapshot()) {
                if (n >= 28) break;
                strncpy(rsp.entries[n].key, kv.first.c_str(), 63);
                rsp.entries[n].expire = htonl((uint32_t)kv.second);
                n++;
            }
            rsp.entry_count = n;
            rsp.result = 0;
            break;
        }
        default:
            rsp.result = 2;
            break;
        }
        rsp.ip_count = htonl((uint32_t)abuse_.ip_blacklist_size());
        rsp.uuid_count = htonl((uint32_t)abuse_.uuid_blacklist_size());
        send_msg(from, MSG_ADMIN_BLACKLIST_RSP, &rsp, sizeof(rsp));
        LOGI("NatServer", "admin blacklist op=%d key=%s result=%d from [%s:%d]",
             req.op, key.c_str(), rsp.result, ipbuf, from_port);
        break;
    }

    // ---------------------------------------------------------------- 注册表同步（NatServer 间，校验 HMAC）
    case MSG_SYNC_PEER_ENTRY: {
        if (plen < sizeof(SyncPeerEntry)) break;
        SyncPeerEntry e;
        memcpy(&e, p, sizeof(SyncPeerEntry));
        e.uuid[MAX_UUID_LEN] = 0;

        // 扩展信息（长度受控；同步载荷尾随 HMAC，签名覆盖 条目+扩展信息）
        uint16_t extlen = ntohs(e.extlen);
        if (extlen > MAX_EXTINFO) break;
        size_t body = sizeof(SyncPeerEntry) + extlen;
        if (plen < body) break;
        if (!sync_verify(p, plen, body)) {
            LOGW("NatServer", "sync entry MAC invalid from [%s:%d]", ipbuf, from_port);
            break;
        }
        std::string extinfo;
        if (extlen > 0) extinfo.assign((const char*)p + sizeof(SyncPeerEntry), extlen);
        handle_sync_entry(e, extinfo, from);
        break;
    }

    case MSG_SYNC_PEER_DEL: {
        if (plen < sizeof(SyncPeerDel)) break;
        SyncPeerDel d;
        memcpy(&d, p, sizeof(SyncPeerDel));
        d.uuid[MAX_UUID_LEN] = 0;
        if (!sync_verify(p, plen, sizeof(SyncPeerDel))) {
            LOGW("NatServer", "sync del MAC invalid from [%s:%d]", ipbuf, from_port);
            break;
        }
        handle_sync_del(d.uuid, from);
        break;
    }

    case MSG_SYNC_SNAPSHOT_REQ: {
        if (plen < sizeof(SyncSnapshotReq)) break;
        if (!sync_verify(p, plen, sizeof(SyncSnapshotReq))) {
            LOGW("NatServer", "sync snapreq MAC invalid from [%s:%d]",
                 ipbuf, from_port);
            break;
        }
        // 防重放：时间戳须在 ±300s 内
        int32_t skew = (int32_t)((uint32_t)time(nullptr) - ntohl(
            reinterpret_cast<const SyncSnapshotReq*>(p)->ts));
        if (skew < -300 || skew > 300) {
            LOGW("NatServer", "sync snapreq stale ts from [%s:%d]", ipbuf, from_port);
            break;
        }
        handle_sync_snapshot_req(from);
        break;
    }

    // ---------------------------------------------------------------- 代理可用性应答
    case MSG_SP_ASK_EXTINFO_RSP: {
        if (plen < sizeof(ProxyAvailRsp)) break;
        ProxyAvailRsp rsp;
        memcpy(&rsp, p, sizeof(rsp));
        collect_proxy_avail(from, rsp);
        LOGI("NatServer", "proxy[%s:%d] used[%d] max[%d] available[%d]",
             ipbuf, from_port, ntohs(rsp.used), ntohs(rsp.max_proxy), rsp.available);
        break;
    }

    default:
        LOGW("NatServer", "invalid msg_id=0x%02x len=[%zu] from [%s:%d]",
             h.msg_id, len, ipbuf, from_port);
        break;
    }
}

} // namespace p2p
