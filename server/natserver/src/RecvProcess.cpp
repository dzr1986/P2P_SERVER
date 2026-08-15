// RecvProcess.cpp：NatServer 报文解析与分发（仿原实现 RecvProcess.cpp）
//   handle_packet 校验报文头后按 msg_id 派发到 on_msg_* 处理器，
//   每个处理器职责单一、可独立测试（C++ 风格重构：拆分巨型 switch）。
#include "NatServer.h"
#include "Crypto.h"
#include "Log.h"
#include "Packet.h"
#include "RegionSched.h"
#include "Uid.h"
#include "Util.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace p2p {

static void copy_uuid(char* dst, const char* src) {
    copy_str_field(dst, MAX_UUID_LEN + 1, src);
}

void NatServer::handle_packet(const uint8_t* data, size_t len, const sockaddr_in& from) {
    PacketReader r(data, len);
    MsgHead h{};
    if (!r.read_struct(h)) return;
    if (ntohs(h.magic) != NAT_MAGIC) return;
    if (h.version != PROTO_VER) return;
    const uint32_t body_len = ntohl(h.length);
    if (body_len > r.remaining()) return;

    const uint8_t* p = data + sizeof(MsgHead);
    const size_t plen = body_len;

    switch (h.msg_id) {
    case MSG_NAT_DETECT_REQ:
        // 已被收包线程快速路径处理，理论上不会到这里；防御性忽略
        break;
    case MSG_HEARTBEAT_REQ:
    case MSG_ADD_UID_REQ:          on_msg_heartbeat_plain(p, plen, from); break;
    case MSG_HEARTBEAT_REQ_ENC:    on_msg_heartbeat_enc(p, plen, from); break;
    case MSG_SND_EXTINFO_REQ:
    case MSG_ASK_EXTINFO_REQ:      on_msg_extinfo(h.msg_id, p, plen, from); break;
    case MSG_CONNECT_REQ:          on_msg_connect_req(p, plen, from); break;
    case MSG_ICE_SDP:              on_msg_ice_sdp(const_cast<uint8_t*>(p), plen, from); break;
    case MSG_GET_DEV_LIST_REQ:     on_msg_dev_list(p, plen, from); break;
    case MSG_GET_SERVER_LIST_REQ:  on_msg_server_list(p, plen, from); break;
    case MSG_DELETE_UID_REQ:       on_msg_delete_uid(p, plen); break;
    case MSG_CHECK_UID_REQ:        on_msg_check_uid(p, plen, from); break;
    case MSG_AUTH_CHALLENGE_REQ:   on_msg_auth_challenge(p, plen, from); break;
    case MSG_AUTH_LOGIN_REQ:       on_msg_auth_login(p, plen, from); break;
    case MSG_ADMIN_STATS_REQ:      on_msg_admin_stats(from); break;
    case MSG_ADMIN_BLACKLIST_REQ:  on_msg_admin_blacklist(p, plen, from); break;
    case MSG_SYNC_PEER_ENTRY:      on_msg_sync_entry(p, plen, from); break;
    case MSG_SYNC_PEER_DEL:        on_msg_sync_del(p, plen, from); break;
    case MSG_SYNC_SNAPSHOT_REQ:    on_msg_sync_snapshot(p, plen, from); break;
    case MSG_SP_ASK_EXTINFO_RSP:   on_msg_proxy_avail(p, plen, from); break;
    default:
        LOGW("NatServer", "invalid msg_id=0x%02x len=[%zu] from [%s]",
             h.msg_id, len, addr_to_str(from).c_str());
        break;
    }
}

// ---------------------------------------------------------------- 心跳/注册
void NatServer::on_msg_heartbeat_plain(const uint8_t* p, size_t plen,
                                       const sockaddr_in& from) {
    PacketReader r(p, plen);
    UuidReq req{};
    if (!r.read_struct(req)) return;
    req.uuid[MAX_UUID_LEN] = 0;

    // 扩展信息（长度受控）
    const uint16_t extlen = ntohs(req.extlen);
    std::string extinfo;
    if (extlen > 0 && extlen <= MAX_EXTINFO && r.remaining() >= extlen) {
        extinfo.assign((const char*)r.cursor(), extlen);
    }
    handle_heartbeat(req, extinfo, from, false, nullptr);
}

void NatServer::on_msg_heartbeat_enc(const uint8_t* p, size_t plen,
                                     const sockaddr_in& from) {
    // 负载 = uuid(33B 明文) || iv(8) || cipher(UuidReq 完整定长 + 扩展信息)
    if (plen < 33 + 8 + sizeof(UuidReq)) return;
    auto cfg = cfg_;

    char uuid[MAX_UUID_LEN + 1];
    memcpy(uuid, p, 33);
    uuid[MAX_UUID_LEN] = 0;
    const uint8_t* iv = p + 33;
    const size_t cipher_len = plen - (33 + 8);

    uint8_t body[MAX_PKT];
    memcpy(body, p + 33 + 8, cipher_len);
    if (!cfg->auth_secret.empty()) {
        // P1：流密钥用每 UID 派生密钥（与 send_heartbeat_rsp 对称）
        uint8_t uid_key[AUTH_KEY_LEN];
        uid_derive_auth_key((const uint8_t*)cfg->auth_secret.data(),
                            cfg->auth_secret.size(), uuid, uid_key);
        p2p_stream_xor(uid_key, AUTH_KEY_LEN, uuid, iv, body, cipher_len);
    }
    // 还原完整 UuidReq（解密后整段即 UuidReq；外层明文 uuid 覆盖防路由篡改）
    UuidReq req{};
    memcpy(&req, body, sizeof(UuidReq));
    memcpy(req.uuid, uuid, MAX_UUID_LEN);
    req.uuid[MAX_UUID_LEN] = 0;

    std::string extinfo;
    if (cipher_len > sizeof(UuidReq)) {
        const size_t ext = cipher_len - sizeof(UuidReq);
        if (ext <= MAX_EXTINFO)
            extinfo.assign((char*)body + sizeof(UuidReq), ext);
    }
    handle_heartbeat(req, extinfo, from, true, iv);
}

// ---------------------------------------------------------------- 地址/扩展信息
void NatServer::on_msg_extinfo(uint8_t msg_id, const uint8_t* p, size_t plen,
                               const sockaddr_in& from) {
    // payload: ExtInfoReq(dst uuid) [+ 上报方扩展信息（SND 时）]
    ExtInfoRsp rsp{};
    inet_ntop(AF_INET, &from.sin_addr, rsp.pub_ip, MAX_IP_LEN);
    rsp.pub_port = from.sin_port;
    rsp.nat_sock2_port = htons(natcheck_.alt_port());

    PacketReader r(p, plen);
    ExtInfoReq req{};
    if (!r.read_struct(req)) {
        send_msg(from, MSG_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
        return;
    }
    req.uuid[MAX_UUID_LEN] = 0;

    if (msg_id == MSG_SND_EXTINFO_REQ) {
        // 上报：更新本机扩展信息，若目标在线则转发
        std::string extinfo;
        if (r.remaining() <= MAX_EXTINFO)
            extinfo.assign((const char*)r.cursor(), r.remaining());
        Peer self;
        if (peers_.get(req.uuid, self)) {
            peers_.upsert(self.uuid, self.pub_addr, self.lan_addr,
                          self.dev_type, self.nattype, extinfo);
        }
        Peer dst;
        if (peers_.get(req.uuid, dst)) {
            send_msg(dst.pub_addr, MSG_SND_EXTINFO_REQ, p, plen);
        }
        return;
    }

    Peer pe;
    if (peers_.get(req.uuid, pe)) {
        inet_ntop(AF_INET, &pe.lan_addr.sin_addr, rsp.lan_ip, MAX_IP_LEN);
        rsp.lan_port = pe.lan_addr.sin_port;
        rsp.result = 0;
        // 目标扩展信息拼在定长应答之后
        uint8_t buf[MAX_PKT];
        PacketWriter w(buf, sizeof(buf));
        w.write_struct(rsp);
        if (!pe.extinfo.empty())
            w.write_bytes(pe.extinfo.data(), pe.extinfo.size());
        send_msg(from, MSG_ASK_EXTINFO_RSP, w.data(), w.size());
    } else {
        rsp.result = 2;   // 目标不在线
        send_msg(from, MSG_ASK_EXTINFO_RSP, &rsp, sizeof(rsp));
    }
}

// ---------------------------------------------------------------- CONNECT 协调
void NatServer::on_msg_connect_req(const uint8_t* p, size_t plen,
                                   const sockaddr_in& from) {
    PacketReader r(p, plen);
    ConnectReq req{};
    if (!r.read_struct(req)) return;
    req.src_uuid[MAX_UUID_LEN] = 0;
    req.dst_uuid[MAX_UUID_LEN] = 0;
    auto cfg = cfg_;

    LOGI("NatServer", "CONNECT_REQ src[%s] dst[%s] from [%s]",
         req.src_uuid, req.dst_uuid, addr_to_str(from).c_str());

    ConnectAck ack{};
    copy_uuid(ack.dst_uuid, req.dst_uuid);

    // 鉴权门
    if (cfg->auth_enabled() && !peers_.authed(req.src_uuid)) {
        ack.result = 3;   // 需鉴权
        send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
        inc_connect_fail();
        return;
    }
    if (cfg->enable_license && !license_.allowed(req.src_uuid)) {
        ack.result = CONNECT_NOT_FOUND;
        send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
        inc_connect_fail();
        return;
    }

    Peer dst;
    if (!peers_.get(req.dst_uuid, dst)) {
        ack.result = CONNECT_NOT_FOUND;
        send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
        LOGI("NatServer", "ack===>dst UUID[%s] not found", req.dst_uuid);
        notify_wake(req.dst_uuid);
        inc_connect_fail();
        return;
    }

    // ---- 应答发起方 ----
    ack.result = CONNECT_OK;
    inet_ntop(AF_INET, &dst.pub_addr.sin_addr, ack.dst_pub_ip, MAX_IP_LEN);
    ack.dst_pub_port = dst.pub_addr.sin_port;
    inet_ntop(AF_INET, &dst.lan_addr.sin_addr, ack.dst_lan_ip, MAX_IP_LEN);
    ack.dst_lan_port = dst.lan_addr.sin_port;
    ack.dst_nattype = dst.nattype;
    pick_proxy(ack.proxies, ack.proxy_count, uid_region(req.src_uuid));
    send_msg(from, MSG_CONNECT_ACK, &ack, sizeof(ack));
    LOGI("NatServer", "ack===>to initiator [%s], dst pub[%s:%d] nattype[%d] proxy[%d]",
         req.src_uuid, ack.dst_pub_ip, ntohs(ack.dst_pub_port), dst.nattype,
         ack.proxy_count);

    // ---- 通知目标方 ----
    ConnectInvite inv{};
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
    pick_proxy(inv.proxies, inv.proxy_count, uid_region(req.dst_uuid));
    send_msg(dst.pub_addr, MSG_CONNECT_INVITE, &inv, sizeof(inv));
    LOGI("NatServer", "invite===>to dst UUID[%s] port:[%d] nattype[%d]",
         req.dst_uuid, ntohs(dst.pub_addr.sin_port), inv.src_nattype);

    inc_connect_ok();
}

// ---------------------------------------------------------------- #19 ICE SDP 中转
void NatServer::on_msg_ice_sdp(uint8_t* p, size_t plen, const sockaddr_in&) {
    // 变长消息：IceSdpMsg { uuid; sdp_len(net); sdp[] }
    if (plen < sizeof(IceSdpMsg) - 1) return;
    auto* m = reinterpret_cast<IceSdpMsg*>(p);
    m->uuid[MAX_UUID_LEN] = 0;
    const uint16_t slen = ntohs(m->sdp_len);
    if (plen < sizeof(IceSdpMsg) - 1 + slen) return;
    Peer dst;
    if (!peers_.get(m->uuid, dst)) {
        LOGW("NatServer", "ICE_SDP dst[%s] not found, drop", m->uuid);
        return;
    }
    send_msg(dst.pub_addr, MSG_ICE_SDP, p, plen);
    LOGI("NatServer", "ICE_SDP===>to dst UUID[%s] len[%d]", m->uuid, (int)plen);
}

// ---------------------------------------------------------------- 设备/服务器列表
void NatServer::on_msg_dev_list(const uint8_t* p, size_t plen,
                                const sockaddr_in& from) {
    PacketReader r(p, plen);
    DevListReq req{};
    r.read_struct(req);   // 负载不足时按零值请求处理（对齐原行为）
    const uint16_t start = ntohs(req.start_index);
    uint16_t want = ntohs(req.want_num);
    if (want == 0 || want > 100) want = 20;

    auto snap = peers_.snapshot();
    std::vector<uint8_t> buf(sizeof(DevListRsp) + want * sizeof(DevListEntry), 0);

    auto* rsp = reinterpret_cast<DevListRsp*>(buf.data());
    rsp->total = htons((uint16_t)snap.size());
    rsp->start_index = htons(start);

    size_t off = sizeof(DevListRsp);
    uint16_t count = 0;
    for (size_t i = start; i < snap.size() && count < want; ++i, ++count) {
        auto* e = reinterpret_cast<DevListEntry*>(buf.data() + off);
        copy_str_field(e->uuid, sizeof(e->uuid), snap[i].uuid.c_str());
        inet_ntop(AF_INET, &snap[i].pub_addr.sin_addr, e->ip, MAX_IP_LEN);
        e->port = snap[i].pub_addr.sin_port;
        e->dev_type = snap[i].dev_type;
        off += sizeof(DevListEntry);
    }
    rsp->count = htons(count);
    send_msg(from, MSG_GET_DEV_LIST_RSP, buf.data(), off);
}

void NatServer::on_msg_server_list(const uint8_t* p, size_t plen, const sockaddr_in& from) {
    auto cfg = cfg_;
    char prefer = 0;
    if (p && plen >= sizeof(ServerListReq)) {
        ServerListReq req{};
        memcpy(&req, p, sizeof(req));
        req.uuid[MAX_UUID_LEN] = 0;
        prefer = uid_region(req.uuid);
    }
    if (!prefer && cfg) prefer = cfg->region;
    const char local = cfg ? cfg->region : 0;

    std::vector<std::string> nats = cfg->nat_ips;
    if (!wan_ip_.empty() && wan_ip_ != "0.0.0.0") {
        bool dup = false;
        for (auto& ip : nats) if (ip == wan_ip_) { dup = true; break; }
        if (!dup) nats.push_back(wan_ip_);
    }
    std::vector<std::string> proxies = cfg->proxy_ips;
    if (cfg) {
        sort_ips_by_region(nats, cfg->nat_regions, prefer, local);
        sort_ips_by_region(proxies, cfg->proxy_regions, prefer, local);
    }

    std::vector<uint8_t> buf(sizeof(ServerListRsp) +
                             (nats.size() + proxies.size()) * MAX_IP_LEN, 0);
    auto* rsp = reinterpret_cast<ServerListRsp*>(buf.data());
    rsp->nat_count = htons((uint16_t)nats.size());
    rsp->proxy_count = htons((uint16_t)proxies.size());

    size_t off = sizeof(ServerListRsp);
    for (auto& ip : nats) {
        copy_str_field(reinterpret_cast<char*>(buf.data() + off), MAX_IP_LEN, ip.c_str());
        off += MAX_IP_LEN;
    }
    for (auto& ip : proxies) {
        copy_str_field(reinterpret_cast<char*>(buf.data() + off), MAX_IP_LEN, ip.c_str());
        off += MAX_IP_LEN;
    }
    send_msg(from, MSG_GET_SERVER_LIST_RSP, buf.data(), off);
}

// ---------------------------------------------------------------- UID 授权
void NatServer::on_msg_delete_uid(const uint8_t* p, size_t plen) {
    UuidReq req{};
    if (plen >= sizeof(UuidReq)) memcpy(&req, p, sizeof(UuidReq));
    else if (plen >= MAX_UUID_LEN + 1) memcpy(req.uuid, p, MAX_UUID_LEN + 1);
    req.uuid[MAX_UUID_LEN] = 0;
    peers_.remove(req.uuid);
    if (license_.remove(req.uuid)) license_.save();
    LOGI("NatServer", "DELETE_UID UUID=[%s]", req.uuid);
}

void NatServer::on_msg_check_uid(const uint8_t* p, size_t plen,
                                 const sockaddr_in& from) {
    PacketReader r(p, plen);
    ExtInfoReq req{};
    r.read_struct(req);   // 负载不足按空 uuid 查询（对齐原行为）
    req.uuid[MAX_UUID_LEN] = 0;
    const uint8_t result = (peers_.exists(req.uuid) || license_.exists(req.uuid)) ? 1 : 0;
    send_msg(from, MSG_CHECK_UID_RSP, &result, sizeof(result));
}

// ---------------------------------------------------------------- 鉴权
void NatServer::on_msg_auth_challenge(const uint8_t* p, size_t plen,
                                      const sockaddr_in& from) {
    PacketReader r(p, plen);
    AuthChallengeReq req{};
    r.read_struct(req);
    req.uuid[MAX_UUID_LEN] = 0;
    auto cfg = cfg_;

    AuthChallengeRsp rsp{};
    rsp.nonce_ttl = htons(30);

    if (abuse_.is_uuid_blacklisted(req.uuid)) { rsp.result = 1; }
    else if (cfg->uid_strict && !uid_valid(req.uuid)) { rsp.result = 2; }   // P1：UID 格式门
    else if (cfg->enable_license && !license_.allowed(req.uuid)) { rsp.result = 2; }
    else if (!issue_auth_nonce(req.uuid, rsp.nonce)) { rsp.result = 3; }
    else { rsp.result = 0; }
    send_msg(from, MSG_AUTH_CHALLENGE_RSP, &rsp, sizeof(rsp));
    LOGI("NatServer", "AUTH_CHALLENGE uuid[%s] result[%d]", req.uuid, rsp.result);
}

void NatServer::on_msg_auth_login(const uint8_t* p, size_t plen,
                                  const sockaddr_in& from) {
    PacketReader r(p, plen);
    AuthLoginReq req{};
    if (!r.read_struct(req)) return;
    req.uuid[MAX_UUID_LEN] = 0;
    auto cfg = cfg_;

    AuthLoginRsp rsp{};
    rsp.session_ttl = htons(3600);
    if (abuse_.is_uuid_blacklisted(req.uuid)) {
        rsp.result = AUTH_BLACKLIST;
        inc_login_fail();
    } else if (cfg->enable_license && !license_.allowed(req.uuid)) {
        rsp.result = AUTH_WHITELIST_REJ;
        inc_login_fail();
    } else if (verify_auth_login(req.uuid, req.nonce, req.mac)) {
        rsp.result = AUTH_OK;
        inc_login_ok();
        LOGI("NatServer", "AUTH_LOGIN uuid[%s] OK", req.uuid);
    } else {
        rsp.result = AUTH_BAD_MAC;
        inc_login_fail();
        LOGW("NatServer", "AUTH_LOGIN uuid[%s] FAIL", req.uuid);
    }
    send_msg(from, MSG_AUTH_LOGIN_RSP, &rsp, sizeof(rsp));
}

// ---------------------------------------------------------------- 管理统计
void NatServer::on_msg_admin_stats(const sockaddr_in& from) {
    AdminStatsRsp rsp{};
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
}

// ---------------------------------------------------------------- 管理黑名单（AdminSecret 校验）
void NatServer::on_msg_admin_blacklist(const uint8_t* p, size_t plen,
                                       const sockaddr_in& from) {
    PacketReader r(p, plen);
    AdminBlacklistReq req{};
    if (!r.read_struct(req)) return;
    auto cfg = cfg_;

    AdminBlacklistRsp rsp{};

    const auto& sec = cfg->admin_secret;
    if (sec.empty()) {
        rsp.result = 1;   // 未配置 AdminSecret，拒绝
        send_msg(from, MSG_ADMIN_BLACKLIST_RSP, &rsp, sizeof(rsp));
        return;
    }
    // mac = HMAC-SHA256(AdminSecret, op(1B) || key(64B 补零))
    uint8_t mac_in[1 + 64];
    mac_in[0] = req.op;
    memcpy(mac_in + 1, req.key, 64);
    uint8_t expect[32];
    hmac_sha256((const uint8_t*)sec.data(), sec.size(), mac_in, sizeof(mac_in), expect);
    if (!p2p_const_time_eq(expect, req.mac, 32)) {
        rsp.result = 1;
        LOGW("NatServer", "admin blacklist bad mac from [%s]", addr_to_str(from).c_str());
        send_msg(from, MSG_ADMIN_BLACKLIST_RSP, &rsp, sizeof(rsp));
        return;
    }

    const std::string key(req.key, strnlen(req.key, 64));
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
            copy_str_field(rsp.entries[n].key, sizeof(rsp.entries[n].key),
                           kv.first.c_str());
            rsp.entries[n].expire = htonl((uint32_t)kv.second);
            n++;
        }
        for (auto& kv : abuse_.uuid_blacklist_snapshot()) {
            if (n >= 28) break;
            copy_str_field(rsp.entries[n].key, sizeof(rsp.entries[n].key),
                           kv.first.c_str());
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
    LOGI("NatServer", "admin blacklist op=%d key=%s result=%d from [%s]",
         req.op, key.c_str(), rsp.result, addr_to_str(from).c_str());
}

// ---------------------------------------------------------------- 注册表同步（NatServer 间，校验 HMAC）
void NatServer::on_msg_sync_entry(const uint8_t* p, size_t plen,
                                  const sockaddr_in& from) {
    PacketReader r(p, plen);
    SyncPeerEntry e{};
    if (!r.read_struct(e)) return;
    e.uuid[MAX_UUID_LEN] = 0;

    // 扩展信息（长度受控；同步载荷尾随 HMAC，签名覆盖 条目+扩展信息）
    const uint16_t extlen = ntohs(e.extlen);
    if (extlen > MAX_EXTINFO) return;
    const size_t body = sizeof(SyncPeerEntry) + extlen;
    if (plen < body) return;
    if (!sync_verify(p, plen, body)) {
        LOGW("NatServer", "sync entry MAC invalid from [%s]", addr_to_str(from).c_str());
        return;
    }
    std::string extinfo;
    if (extlen > 0) extinfo.assign((const char*)p + sizeof(SyncPeerEntry), extlen);
    handle_sync_entry(e, extinfo, from);
}

void NatServer::on_msg_sync_del(const uint8_t* p, size_t plen,
                                const sockaddr_in& from) {
    PacketReader r(p, plen);
    SyncPeerDel d{};
    if (!r.read_struct(d)) return;
    d.uuid[MAX_UUID_LEN] = 0;
    if (!sync_verify(p, plen, sizeof(SyncPeerDel))) {
        LOGW("NatServer", "sync del MAC invalid from [%s]", addr_to_str(from).c_str());
        return;
    }
    handle_sync_del(d.uuid, from);
}

void NatServer::on_msg_sync_snapshot(const uint8_t* p, size_t plen,
                                     const sockaddr_in& from) {
    if (plen < sizeof(SyncSnapshotReq)) return;
    if (!sync_verify(p, plen, sizeof(SyncSnapshotReq))) {
        LOGW("NatServer", "sync snapreq MAC invalid from [%s]", addr_to_str(from).c_str());
        return;
    }
    // 防重放：时间戳须在 ±300s 内
    const int32_t skew = (int32_t)((uint32_t)time(nullptr) - ntohl(
        reinterpret_cast<const SyncSnapshotReq*>(p)->ts));
    if (skew < -300 || skew > 300) {
        LOGW("NatServer", "sync snapreq stale ts from [%s]", addr_to_str(from).c_str());
        return;
    }
    handle_sync_snapshot_req(from);
}

// ---------------------------------------------------------------- 代理可用性应答
void NatServer::on_msg_proxy_avail(const uint8_t* p, size_t plen,
                                   const sockaddr_in& from) {
    PacketReader r(p, plen);
    ProxyAvailRsp rsp{};
    if (!r.read_struct(rsp)) return;
    collect_proxy_avail(from, rsp);
    LOGI("NatServer", "proxy[%s] used[%d] max[%d] available[%d]",
         addr_to_str(from).c_str(), ntohs(rsp.used), ntohs(rsp.max_proxy),
         rsp.available);
}

} // namespace p2p
