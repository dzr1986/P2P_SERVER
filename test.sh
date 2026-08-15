#!/bin/bash
# 本地端到端测试：NAT 打洞直连 + 中继兜底 + 鉴权 + NAT 类型检测
# 注意：测试端口用 18832/18833，避免与已运行的 8832/8833 原版进程冲突。
cd "$(dirname "$0")"

NAT_PORT=18832
NAT_PORT2=18834
PROXY_PORT=18833
NAT_BIN=./server/natserver/bin/p2p_natserver
PROXY_BIN=./server/proxyserver/bin/p2p_proxy
PEER_BIN=./client/bin/peer
IOTC_BIN=./client/bin/iotc_demo
WAKE_BIN=./server/wakeserver/bin/p2p_wakeserver
TOKENGEN=./tools/bin/tokengen
CFG=/tmp/p2p_auth_test.cfg

PASS=0
FAIL=0
FAIL_MSG=""

fail() { FAIL=$((FAIL+1)); FAIL_MSG="$FAIL_MSG\n  $1"; }
ok()   { PASS=$((PASS+1)); echo "  ok: $1"; }

# 本环境没有 killall；pkill -f 用 [x] 避免匹配到本行。
# SO_REUSEPORT 残留会把 UDP 分给旧 NatServer，表现为随机鉴权失败/直连超时。
kill_strays() {
    pkill -9 -f 'server/natserver/bin/[p]2p_natserver' 2>/dev/null || true
    pkill -9 -f 'server/proxyserver/bin/[p]2p_proxy' 2>/dev/null || true
    pkill -9 -f 'server/wakeserver/bin/[p]2p_wakeserver' 2>/dev/null || true
    pkill -9 -f 'client/bin/[p]eer' 2>/dev/null || true
    pkill -9 -f 'client/bin/[i]otc_demo' 2>/dev/null || true
}

# 统一清理
cleanup() {
    for p in $NAT_PID $NAT2_PID $PROXY_PID $PA_PID $PB_PID $IOTC_DEV_PID $WAKE_PID; do
        [ -n "$p" ] && kill -9 $p 2>/dev/null
    done
    kill_strays
}
trap cleanup EXIT

start_servers() {
    local cfg="$1"
    local args=""
    [ -n "$cfg" ] && args=" $cfg"
    stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1$args > /tmp/nat.log 2>&1 &
    NAT_PID=$!
    sleep 0.5
    stdbuf -oL $PROXY_BIN $PROXY_PORT 100 > /tmp/proxy.log 2>&1 &
    PROXY_PID=$!
    sleep 0.5
}

stop_servers() {
    kill -9 $NAT_PID $PROXY_PID 2>/dev/null
    sleep 0.3
    NAT_PID=""; PROXY_PID=""
}

stop_all() {
    kill -9 $NAT_PID $NAT2_PID $PROXY_PID 2>/dev/null
    sleep 0.3
    NAT_PID=""; NAT2_PID=""; PROXY_PID=""
}

echo "== build =="
make -s all || { echo "BUILD FAILED"; exit 1; }
echo "build ok"
# 测试默认关闭 LAN 组播，避免占用 17890 / 串扰 ICE；[15] 再打开
export P2P_DISABLE_LAN=1
export P2P_DISABLE_PORTMAP=1   # CI 无家宽 IGD，开孔单测见 [0] portmap_test
kill_strays
sleep 0.3

# ---------------------------------------------------------------- 0. 单元测试
echo "== [0] unit tests =="
./tests/bin/crypto_test > /tmp/crypto_test.log 2>&1 && ok "crypto unit tests" || fail "crypto unit tests"
./tests/bin/session_test > /tmp/session_test.log 2>&1 && ok "session unit tests" || fail "session unit tests"
./tests/bin/uid_test > /tmp/uid_test.log 2>&1 && ok "uid unit tests" || fail "uid unit tests"
./tests/bin/token_test > /tmp/token_test.log 2>&1 && ok "connect token unit tests" || fail "connect token unit tests"
./tests/bin/av_frame_test > /tmp/av_frame_test.log 2>&1 && ok "av/tunnel codec unit tests" || fail "av/tunnel codec unit tests"
./tests/bin/sched_test > /tmp/sched_test.log 2>&1 && ok "region scheduler unit tests" || fail "region scheduler unit tests"
./tests/bin/stun_test > /tmp/stun_test.log 2>&1 && ok "STUN Binding unit tests" || fail "STUN Binding unit tests"
./tests/bin/abr_test > /tmp/abr_test.log 2>&1 && ok "ABR delay-based unit tests" || fail "ABR delay-based unit tests"
./tests/bin/ice_sdp_test > /tmp/ice_sdp_test.log 2>&1 && ok "ICE SDP ufrag/restart unit tests" || fail "ICE SDP unit tests"
./tests/bin/twcc_test > /tmp/twcc_test.log 2>&1 && ok "TWCC Kalman unit tests" || fail "TWCC unit tests"
./tests/bin/portmap_test > /tmp/portmap_test.log 2>&1 && ok "portmap NAT-PMP/UPnP unit tests" || fail "portmap unit tests"
./tests/bin/nat_detect_test > /tmp/nat_detect_test.log 2>&1 && ok "RFC 4787 NAT detect / matrix unit tests" || fail "nat detect unit tests"

# ---------------------------------------------------------------- 1. 直连
echo "== [1] direct P2P (no auth) =="
start_servers ""
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 8
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "CONNECTED to B via direct" /tmp/peerA.log && ok "A->B direct" || fail "A->B direct missing"
grep -q "CONNECTED to A via direct" /tmp/peerB.log && ok "B->A direct" || fail "B->A direct missing"
grep -q "nattype=full-cone" /tmp/peerA.log && ok "A nattype detected" || fail "A nattype missing"
grep -q "nattype=full-cone" /tmp/peerB.log && ok "B nattype detected" || fail "B nattype missing"
grep -q "recv from B" /tmp/peerA.log && ok "A received B data" || fail "A data missing"
stop_servers

# ---------------------------------------------------------------- 2. 中继兜底
echo "== [2] relay fallback (-relay) =="
start_servers ""
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A 127.0.0.1 $PROXY_PORT -relay > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 8
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "CONNECTED to B via relay" /tmp/peerA.log && ok "A->B relay" || fail "A->B relay missing"
grep -q "CONNECTED to A via relay" /tmp/peerB.log && ok "B->A relay" || fail "B->A relay missing"
grep -q "register ok" /tmp/proxy.log && ok "proxy registration" || fail "proxy registration missing"
stop_servers

# ---------------------------------------------------------------- 3. 鉴权 + 加密心跳 + 多收包线程
echo "== [3] auth + encrypted heartbeat (EnableAuth=1, RecvThreads=3) =="
printf 'AuthSecret=s3cr3t\nEnableAuth=1\nRecvThreads=3\nProcWorkers=2\n' > $CFG
start_servers $CFG
grep -q "recv_threads\[3\]" /tmp/nat.log && ok "multi-recv threads (3)" || fail "recv_threads not started"
grep -q "recv clone socket" /tmp/nat.log && ok "recv clone sockets" || fail "recv clone sockets missing"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT C -s s3cr3t > /tmp/peerC.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT D C 127.0.0.1 $PROXY_PORT -s s3cr3t > /tmp/peerD.log 2>&1 & PB_PID=$!
sleep 6
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "AUTH_LOGIN uuid\[C\] OK" /tmp/nat.log && ok "C auth login" || fail "C auth login missing"
grep -q "CONNECTED to C via direct" /tmp/peerD.log && ok "D->C direct (auth)" || fail "D->C direct missing"
grep -q "heartbeat/register UUID=\[C\] .*enc=1" /tmp/nat.log && ok "C encrypted heartbeat accepted" || fail "encrypted heartbeat missing"
# 未配置密钥应被拒绝
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT E > /tmp/peerE.log 2>&1 & PA_PID=$!
sleep 3
kill -9 $PA_PID 2>/dev/null; PA_PID=""
grep -q "requires auth" /tmp/peerE.log && ok "E rejected without secret" || fail "E not rejected"
stop_servers

# ---------------------------------------------------------------- 4. UUID 白名单鉴权
echo "== [4] UUID whitelist (EnableLicense=1 + AllowedUuids) =="
printf 'AuthSecret=s3cr3t\nEnableAuth=1\nEnableLicense=1\nAllowedUuids=WW,XX\n' > $CFG
start_servers $CFG
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT WW -s s3cr3t > /tmp/peerW.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT YY -s s3cr3t > /tmp/peerY.log 2>&1 & PB_PID=$!
sleep 4
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "ready" /tmp/peerW.log && ok "WW (whitelisted) registered" || fail "WW not registered"
grep -q "not in whitelist" /tmp/peerY.log && ok "YY rejected by whitelist" || fail "YY not rejected"
grep -q "AUTH_LOGIN uuid\[WW\] OK" /tmp/nat.log && ok "WW auth OK" || fail "WW auth missing"
stop_servers

# ---------------------------------------------------------------- 5. 打洞数据面加密
echo "== [5] tunnel data-plane encryption (auth + direct) =="
printf 'AuthSecret=s3cr3t\nEnableAuth=1\n' > $CFG
start_servers $CFG
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT F -s s3cr3t > /tmp/peerF.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT G F -s s3cr3t > /tmp/peerG.log 2>&1 & PB_PID=$!
sleep 7
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "CONNECTED to F via direct" /tmp/peerG.log && ok "G->F direct (enc)" || fail "G->F direct missing"
grep -q "recv from F" /tmp/peerG.log && ok "G decrypted F data" || fail "G did not decrypt F data"
grep -q "tunnel-enc tx=[1-9]" /tmp/peerF.log && ok "F frames encrypted on wire" || fail "F no encrypted tx"
grep -q "tunnel-enc tx=[1-9]" /tmp/peerG.log && ok "G frames encrypted on wire" || fail "G no encrypted tx"
grep -q "rx=[1-9]" /tmp/peerG.log && ok "G decrypted inbound frames" || fail "G no decrypted rx"
grep -q "fs-hs=[1-9]" /tmp/peerF.log && ok "F X25519 FS handshake" || fail "F FS handshake missing"
grep -q "fs-hs=[1-9]" /tmp/peerG.log && ok "G X25519 FS handshake" || fail "G FS handshake missing"
stop_servers

# ---------------------------------------------------------------- 6. 多 NatServer 注册表同步
echo "== [6] multi-NatServer registry sync (SyncPeers=1) =="
printf 'SyncPeers=1\nSyncAddrs=127.0.0.1:%d\n' $NAT_PORT2 > $CFG
NAT_PID=""
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
printf 'SyncPeers=1\nSyncAddrs=127.0.0.1:%d\n' $NAT_PORT > /tmp/p2p_auth_test2.cfg
stdbuf -oL $NAT_BIN $NAT_PORT2 $PROXY_PORT 127.0.0.1 /tmp/p2p_auth_test2.cfg > /tmp/nat2.log 2>&1 &
NAT2_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5

grep -q "registry sync enabled, 1 peer(s)" /tmp/nat.log && ok "S1 sync peers" || fail "S1 sync peers missing"
grep -q "registry sync enabled, 1 peer(s)" /tmp/nat2.log && ok "S2 sync peers" || fail "S2 sync peers missing"

# A 注册到 S1，B 注册到 S2 并连接 A（A 在 S1，B 在 S2，跨服协调）
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT2 B A 127.0.0.1 $PROXY_PORT > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 8
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""

grep -q "sync entry uuid\[A\]" /tmp/nat2.log && ok "S2 learned A via sync" || fail "S2 did not sync A"
grep -q "CONNECTED to A via direct" /tmp/peerB.log && ok "B->A direct (cross-server)" || fail "B->A cross-server direct missing"
grep -q "sync snapshot sent" /tmp/nat.log && ok "S1 snapshot response" || fail "S1 snapshot response missing"
stop_all

# 管理黑名单客户端（op=1加IP 2删IP 3加UUID 4删UUID 5查询；badmac 测试错误 MAC）
cat > /tmp/p2p_admin.py <<'PYEOF'
import socket, struct, sys, hmac, hashlib
port, op, key, secret = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
badmac = len(sys.argv) > 5 and sys.argv[5] == "badmac"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2); s.bind(("127.0.0.1", 0))
mac = hmac.new(secret.encode(), bytes([op]) + key.encode().ljust(64, b"\x00")[:64], hashlib.sha256).digest()
if badmac: mac = bytes([mac[0] ^ 0xFF]) + mac[1:]
payload = bytes([op]) + key.encode().ljust(64, b"\x00")[:64] + mac
s.sendto(struct.pack(">HBBI", 0x584E, 0x02, 0x1D, len(payload)) + payload, ("127.0.0.1", port))
data, _ = s.recvfrom(4096)
body = data[8:]
result = body[0]; ip_count, uuid_count = struct.unpack(">II", body[1:9]); n = body[9]
keys = []
off = 10
for _ in range(n):
    k = body[off:off+64].split(b"\x00")[0].decode(); keys.append(k); off += 68
print("RESULT=%d IP=%d UUID=%d N=%d KEYS=%s" % (result, ip_count, uuid_count, n, ",".join(keys)))
PYEOF

# 伪造同步条目（错误 MAC）发送给指定端口
cat > /tmp/p2p_forge.py <<'PYEOF'
import socket, struct, sys, time
port = int(sys.argv[1]); uuid = sys.argv[2].encode().ljust(33, b"\x00")
entry = uuid + b"127.0.0.1".ljust(16, b"\x00") + struct.pack(">H", 55555) \
      + b"127.0.0.1".ljust(16, b"\x00") + struct.pack(">H", 55556) \
      + bytes([2, 1, 1, 0]) + struct.pack(">I", int(time.time())) + struct.pack(">H", 0)
entry += b"\x00" * 32   # 伪造 MAC
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(struct.pack(">HBBI", 0x584E, 0x02, 0x30, len(entry)) + entry, ("127.0.0.1", port))
PYEOF

# ---------------------------------------------------------------- 7. 黑名单持久化 + 管理接口
echo "== [7] blacklist persistence + admin mgmt (BlacklistFile/AdminSecret) =="
rm -f /tmp/p2p_bl.cfg
printf 'BlacklistFile=/tmp/p2p_bl.cfg\nAdminSecret=admsecret\nBlacklistSeconds=600\n' > $CFG
start_servers $CFG
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 1 10.0.0.9 admsecret)
echo "$ADM" | grep -q "RESULT=0" && ok "admin add IP" || fail "admin add IP ($ADM)"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 3 ZZADMIN admsecret)
echo "$ADM" | grep -q "RESULT=0" && ok "admin add UUID" || fail "admin add UUID ($ADM)"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 5 x admsecret)
echo "$ADM" | grep -q "IP=1 UUID=1" && ok "admin query counts (1/1)" || fail "admin query counts ($ADM)"
echo "$ADM" | grep -q "10.0.0.9" && ok "query lists IP" || fail "query IP missing ($ADM)"
echo "$ADM" | grep -q "ZZADMIN" && ok "query lists UUID" || fail "query UUID missing ($ADM)"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 1 10.0.0.8 admsecret badmac)
echo "$ADM" | grep -q "RESULT=1" && ok "bad admin mac rejected" || fail "bad mac not rejected ($ADM)"
sleep 2   # 等待定时器落盘
stop_servers
sleep 0.5
grep -q "IP 10.0.0.9" /tmp/p2p_bl.cfg && ok "blacklist file has IP" || fail "blacklist file missing IP"
grep -q "UUID ZZADMIN" /tmp/p2p_bl.cfg && ok "blacklist file has UUID" || fail "blacklist file missing UUID"
start_servers $CFG
sleep 0.5
grep -q "loaded blacklist" /tmp/nat.log && ok "blacklist reloaded on boot" || fail "blacklist not reloaded"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 5 x admsecret)
echo "$ADM" | grep -q "IP=1 UUID=1" && ok "persisted counts after restart (1/1)" || fail "persist counts ($ADM)"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 2 10.0.0.9 admsecret)
echo "$ADM" | grep -q "RESULT=0" && ok "admin del IP" || fail "admin del IP ($ADM)"
ADM=$(python3 /tmp/p2p_admin.py $NAT_PORT 5 x admsecret)
echo "$ADM" | grep -q "IP=0" && ok "IP removed after del" || fail "IP not removed ($ADM)"
stop_servers

# ---------------------------------------------------------------- 8. 跨服同步鉴权（SyncAuthSecret）
echo "== [8] cross-server sync auth (SyncAuthSecret HMAC) =="
printf 'SyncPeers=1\nSyncAddrs=127.0.0.1:%d\nSyncAuthSecret=sharedsec\n' $NAT_PORT2 > $CFG
NAT_PID=""
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
printf 'SyncPeers=1\nSyncAddrs=127.0.0.1:%d\nSyncAuthSecret=sharedsec\n' $NAT_PORT > /tmp/p2p_auth_test2.cfg
stdbuf -oL $NAT_BIN $NAT_PORT2 $PROXY_PORT 127.0.0.1 /tmp/p2p_auth_test2.cfg > /tmp/nat2.log 2>&1 &
NAT2_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5

stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA8.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT2 B A 127.0.0.1 $PROXY_PORT > /tmp/peerB8.log 2>&1 & PB_PID=$!
sleep 10
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "sync entry uuid\[A\]" /tmp/nat2.log && ok "signed sync entry accepted" || fail "signed sync not accepted"
grep -q "CONNECTED to A via direct" /tmp/peerB8.log && ok "B->A direct (signed sync)" || fail "cross-server direct missing ($([ -f /tmp/peerB8.log ] && tail -20 /tmp/peerB8.log))"
python3 /tmp/p2p_forge.py $NAT_PORT2 HACKER1
sleep 1
grep -q "sync entry MAC invalid" /tmp/nat2.log && ok "forged sync entry rejected" || fail "forged sync entry not rejected"
stop_all

# ---------------------------------------------------------------- 9. UID 体系（UidStrict + 每 UID AuthKey）
echo "== [9] structured UID (UidStrict=1 + per-UID AuthKey) =="
MASTER=master-secret-2026
UIDLINE=$(./tools/bin/uidgen CAMA C 1 $MASTER)
DEV_UID=$(echo "$UIDLINE" | awk '{print $1}')
DEV_KEY=$(echo "$UIDLINE" | awk '{print $2}')
[ ${#DEV_UID} -eq 20 ] && [ ${#DEV_KEY} -eq 64 ] && ok "uidgen output (uid=$DEV_UID)" || fail "uidgen output malformed"

printf 'NatServer1=127.0.0.1\nProxy1_1=127.0.0.1\nAuthSecret=%s\nEnableAuth=1\nUidStrict=1\n' $MASTER > $CFG
start_servers "$CFG"
# 合法 UID + uidgen 签发的 AuthKey（设备侧不知晓主密钥，仅持 -k）
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$DEV_UID" -k "$DEV_KEY" > /tmp/peerU.log 2>&1 & PA_PID=$!
sleep 2
# 非法 UID（非结构化）应被拒绝
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT BADUID -s $MASTER > /tmp/peerV.log 2>&1 & PB_PID=$!
sleep 2
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "AUTH_LOGIN uuid\[$DEV_UID\] OK" /tmp/nat.log && ok "valid UID auth via per-UID key" || fail "valid UID auth missing"
grep -q "ready pub=" /tmp/peerU.log && ok "valid UID registered" || fail "valid UID not registered"
grep -qE "uuid not in whitelist|rejected" /tmp/peerV.log && ok "invalid UID rejected" || fail "invalid UID not rejected"
if grep -q "AUTH_LOGIN uuid\[BADUID\] OK" /tmp/nat.log; then fail "invalid UID wrongly authed"; else ok "invalid UID never authed"; fi
stop_servers

# ---------------------------------------------------------------- 10. IOTC/AV/RDT/Tunnel 四通道 + TCP 映射
echo "== [10] IOTC/AV/RDT/Tunnel 4-channel echo =="
start_servers ""
stdbuf -oL $IOTC_BIN device 127.0.0.1 $NAT_PORT IOTCDEV > /tmp/iotc_dev.log 2>&1 &
IOTC_DEV_PID=$!
sleep 1
stdbuf -oL $IOTC_BIN client 127.0.0.1 $NAT_PORT IOTCCLI IOTCDEV > /tmp/iotc_cli.log 2>&1
IOTC_RC=$?
kill -9 $IOTC_DEV_PID 2>/dev/null; IOTC_DEV_PID=""
[ $IOTC_RC -eq 0 ] && grep -q "4-channel echo PASS" /tmp/iotc_cli.log && ok "iotc client exit 0" || fail "iotc client failed (rc=$IOTC_RC)"
grep -q "ioctl echo ok" /tmp/iotc_cli.log && ok "IOCtrl echo" || fail "IOCtrl echo missing"
grep -q "video echo ok frames=5" /tmp/iotc_cli.log && ok "video frame echo" || fail "video frame echo missing"
grep -q "audio echo ok frames=5" /tmp/iotc_cli.log && ok "audio frame echo" || fail "audio frame echo missing"
grep -q "rdt echo ok" /tmp/iotc_cli.log && ok "RDT byte-stream echo" || fail "RDT echo missing"
grep -q "tunnel echo ok" /tmp/iotc_cli.log && ok "P2PTunnel TCP echo" || fail "P2PTunnel echo missing"
grep -q "device session sid=" /tmp/iotc_dev.log && ok "device listen accepted" || fail "device listen missing"
stop_servers

# ---------------------------------------------------------------- 11. IOTC 强制中继四通道
echo "== [11] IOTC 4-channel via relay =="
start_servers ""
stdbuf -oL $IOTC_BIN device 127.0.0.1 $NAT_PORT IOTCDEV -relay 127.0.0.1 $PROXY_PORT > /tmp/iotc_dev.log 2>&1 &
IOTC_DEV_PID=$!
sleep 1
stdbuf -oL $IOTC_BIN client 127.0.0.1 $NAT_PORT IOTCCLI IOTCDEV -relay 127.0.0.1 $PROXY_PORT > /tmp/iotc_cli.log 2>&1
IOTC_RC=$?
kill -9 $IOTC_DEV_PID 2>/dev/null; IOTC_DEV_PID=""
[ $IOTC_RC -eq 0 ] && grep -q "4-channel echo PASS" /tmp/iotc_cli.log && ok "iotc relay client exit 0" || fail "iotc relay client failed (rc=$IOTC_RC)"
grep -q "relay=1" /tmp/iotc_cli.log && ok "client via relay" || fail "client not via relay"
grep -q "rdt echo ok" /tmp/iotc_cli.log && ok "RDT echo over relay" || fail "RDT relay echo missing"
grep -q "tunnel echo ok" /tmp/iotc_cli.log && ok "P2PTunnel over relay" || fail "tunnel relay echo missing"
stop_servers

# ---------------------------------------------------------------- 12. P6 就近调度 + Prometheus
echo "== [12] region schedule + Prometheus /metrics =="
STATUS_PORT=18890
export P2P_STATUS_PORT=$STATUS_PORT
printf 'NatServer1=10.0.0.1\nNatServer2=10.0.0.2\nNatServer3=127.0.0.1\nProxy1_1=10.1.0.1\nProxy1_2=127.0.0.1\nRegion=C\nNatRegions=10.0.0.1:A,10.0.0.2:B,127.0.0.1:C\nProxyRegions=10.1.0.1:A,127.0.0.1:C\n' > $CFG
start_servers "$CFG"
sleep 0.3
STUN=$(python3 - <<PY
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2); s.bind(("127.0.0.1", 0))
req = struct.pack("!HHI", 0x0001, 0, 0x2112A442) + b"\\x11" * 12
s.sendto(req, ("127.0.0.1", $NAT_PORT))
data, _ = s.recvfrom(64)
typ, = struct.unpack("!H", data[:2])
xport, = struct.unpack("!H", data[26:28])
xaddr, = struct.unpack("!I", data[28:32])
port = xport ^ 0x2112
addr = xaddr ^ 0x2112A442
ip = socket.inet_ntoa(struct.pack("!I", addr))
print("STUN typ=0x%04x ip=%s port=%d" % (typ, ip, port))
PY
)
echo "$STUN" | grep -q "typ=0x0101" && ok "NatServer STUN Binding success" || fail "STUN Binding ($STUN)"
MET=$(python3 - <<PY
import urllib.request
print(urllib.request.urlopen("http://127.0.0.1:$STATUS_PORT/metrics", timeout=2).read().decode())
PY
)
echo "$MET" | grep -q "p2p_online_peers" && ok "prometheus p2p_online_peers" || fail "metrics missing p2p_online_peers"
echo "$MET" | grep -q "p2p_connect_ok_total" && ok "prometheus connect counter" || fail "metrics missing connect counter"
echo "$MET" | grep -q 'p2p_node_region{region="C"}' && ok "prometheus region label" || fail "metrics missing region"
JS=$(python3 - <<PY
import urllib.request
print(urllib.request.urlopen("http://127.0.0.1:$STATUS_PORT/", timeout=2).read().decode())
PY
)
echo "$JS" | grep -q '"region":"C"' && ok "json region=C" || fail "json region missing ($JS)"
echo "$JS" | grep -q '"uptime_seconds"' && ok "json uptime" || fail "json uptime missing"

UIDA=$(./tools/bin/uidgen CAMA A 1 master-secret-2026 | awk '{print $1}')
UIDC=$(./tools/bin/uidgen CAMA C 1 master-secret-2026 | awk '{print $1}')
cat > /tmp/p2p_slist.py <<'PYEOF'
import socket, struct, sys
port, uid = int(sys.argv[1]), sys.argv[2]
payload = uid.encode().ljust(33, b"\x00")[:33]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2); s.bind(("127.0.0.1", 0))
s.sendto(struct.pack(">HBBI", 0x584E, 0x02, 0x0B, len(payload)) + payload, ("127.0.0.1", port))
data, _ = s.recvfrom(4096)
body = data[8:]
nat_n, proxy_n = struct.unpack(">HH", body[:4])
off = 4
nats = []
for _ in range(nat_n):
    nats.append(body[off:off+16].split(b"\x00")[0].decode()); off += 16
proxies = []
for _ in range(proxy_n):
    proxies.append(body[off:off+16].split(b"\x00")[0].decode()); off += 16
print("NATS=" + ",".join(nats))
print("PROXIES=" + ",".join(proxies))
PYEOF
LA=$(python3 /tmp/p2p_slist.py $NAT_PORT "$UIDA")
LC=$(python3 /tmp/p2p_slist.py $NAT_PORT "$UIDC")
echo "$LA" | grep -q "NATS=10.0.0.1," && ok "GET_SERVER_LIST region A prefers 10.0.0.1" || fail "region A sort ($LA)"
echo "$LC" | grep -q "NATS=127.0.0.1," && ok "GET_SERVER_LIST region C prefers 127.0.0.1" || fail "region C sort ($LC)"
echo "$LA" | grep -q "PROXIES=10.1.0.1," && ok "proxy list region A prefers 10.1.0.1" || fail "proxy A sort ($LA)"
echo "$LC" | grep -q "PROXIES=127.0.0.1," && ok "proxy list region C prefers 127.0.0.1" || fail "proxy C sort ($LC)"
unset P2P_STATUS_PORT
stop_servers

# ---------------------------------------------------------------- 13. P7 wakeserver 保活/唤醒
echo "== [13] wakeserver keepalive + poke =="
WAKE_PORT=18840
stdbuf -oL $WAKE_BIN $WAKE_PORT wakesec > /tmp/wake.log 2>&1 &
WAKE_PID=$!
sleep 0.4
cat > /tmp/p2p_wake.py <<'PYEOF'
import socket, struct, sys, hmac, hashlib
port, op, uid, secret = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
mac = hmac.new(secret.encode(), uid.encode(), hashlib.sha256).digest()
if op == "badmac":
    mac = bytes([mac[0] ^ 0xFF]) + mac[1:]
    op = "keep"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
s.bind(("127.0.0.1", 0))
if op == "keep":
    payload = uid.encode().ljust(33, b"\x00")[:33] + mac
    mid = 0x40
elif op == "trigger":
    payload = uid.encode().ljust(33, b"\x00")[:33]
    mid = 0x41
else:
    sys.exit(2)
s.sendto(struct.pack(">HBBI", 0x584E, 0x02, mid, len(payload)) + payload, ("127.0.0.1", port))
data, _ = s.recvfrom(4096)
body = data[8:]
print("MSG=0x%02x RESULT=%d UID=%s" % (data[3], body[0], body[1:34].split(b"\x00")[0].decode()))
if op == "keep" and body[0] == 0:
    # 等 POKE（由另一次 trigger 触发）
    try:
        poke, _ = s.recvfrom(4096)
        print("POKE=0x%02x UID=%s" % (poke[3], poke[9:42].split(b"\x00")[0].decode()))
    except socket.timeout:
        print("POKE=none")
PYEOF
# 先单独测坏 MAC / 未报到 TRIGGER（不占用保活 socket）
BAD=$(python3 /tmp/p2p_wake.py $WAKE_PORT badmac SLEEPDEV wakesec)
echo "$BAD" | grep -q "RESULT=2" && ok "wake keep bad mac rejected" || fail "wake bad mac ($BAD)"
NF=$(python3 /tmp/p2p_wake.py $WAKE_PORT trigger NOSUCH wakesec)
echo "$NF" | grep -q "RESULT=1" && ok "wake trigger unknown" || fail "wake unknown ($NF)"

# 保活 socket 挂起等 POKE：后台 keep，再 trigger
python3 - <<PY > /tmp/wake_dev.log 2>&1 &
import socket, struct, hmac, hashlib, time
port, uid, secret = $WAKE_PORT, "SLEEPDEV", "wakesec"
mac = hmac.new(secret.encode(), uid.encode(), hashlib.sha256).digest()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(4)
s.bind(("127.0.0.1", 0))
payload = uid.encode().ljust(33, b"\x00")[:33] + mac
s.sendto(struct.pack(">HBBI", 0x584E, 0x02, 0x40, len(payload)) + payload, ("127.0.0.1", port))
ack, _ = s.recvfrom(4096)
print("KEEP_RESULT=%d" % ack[8])
poke, _ = s.recvfrom(4096)
print("POKE=0x%02x" % poke[3])
PY
WDEV=$!
sleep 0.4
TR=$(python3 /tmp/p2p_wake.py $WAKE_PORT trigger SLEEPDEV wakesec)
echo "$TR" | grep -q "RESULT=0" && ok "wake trigger ok" || fail "wake trigger ($TR)"
wait $WDEV 2>/dev/null
grep -q "KEEP_RESULT=0" /tmp/wake_dev.log && ok "wake keep accepted" || fail "wake keep missing"
grep -q "POKE=0x43" /tmp/wake_dev.log && ok "device received POKE" || fail "device poke missing ($(cat /tmp/wake_dev.log))"
grep -q "KEEP uuid\[SLEEPDEV\]" /tmp/wake.log && ok "wakeserver logged keep" || fail "wake log keep missing"
grep -q "TRIGGER uuid\[SLEEPDEV\] poke" /tmp/wake.log && ok "wakeserver logged poke" || fail "wake log poke missing"

# NatServer CONNECT 离线目标时转发 TRIGGER
printf 'WakeServer=127.0.0.1:%s\n' $WAKE_PORT > $CFG
start_servers "$CFG"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT WAKECLI SLEEPDEV > /tmp/peerWake.log 2>&1 & PA_PID=$!
sleep 3
kill -9 $PA_PID 2>/dev/null; PA_PID=""
grep -q "wake trigger uuid\[SLEEPDEV\]" /tmp/nat.log && ok "nat notify wakeserver" || fail "nat wake notify missing"
grep -q "TRIGGER uuid\[SLEEPDEV\]" /tmp/wake.log && ok "wakeserver got nat trigger" || fail "wake nat trigger missing"
stop_servers
kill -9 $WAKE_PID 2>/dev/null; WAKE_PID=""

# ---------------------------------------------------------------- 14. 连线 Token（EnableConnectToken）
echo "== [14] connect token (EnableConnectToken=1) =="
MASTER=master-secret-2026
DEVLINE=$(./tools/bin/uidgen CAMA C 1 $MASTER)
CLILINE=$(./tools/bin/uidgen CAMA C 1 $MASTER)
DEV_UID=$(echo "$DEVLINE" | awk '{print $1}')
DEV_KEY=$(echo "$DEVLINE" | awk '{print $2}')
CLI_UID=$(echo "$CLILINE" | awk '{print $1}')
CLI_KEY=$(echo "$CLILINE" | awk '{print $2}')
TOK=$($TOKENGEN "$DEV_UID" "$CLI_UID" 300 $MASTER)
EXPT=$($TOKENGEN "$DEV_UID" "$CLI_UID" 0 $MASTER)
OTH=$(./tools/bin/uidgen CAMA A 1 $MASTER | awk '{print $1}')
WRONGT=$($TOKENGEN "$DEV_UID" "$OTH" 300 $MASTER)
[ ${#TOK} -eq 104 ] && ok "tokengen output (104 hex)" || fail "tokengen malformed ($TOK)"

printf 'NatServer1=127.0.0.1\nProxy1_1=127.0.0.1\nAuthSecret=%s\nEnableAuth=1\nUidStrict=1\nEnableConnectToken=1\n' $MASTER > $CFG
start_servers "$CFG"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$DEV_UID" -k "$DEV_KEY" > /tmp/peerTokDev.log 2>&1 & PA_PID=$!
sleep 2
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$CLI_UID" "$DEV_UID" -k "$CLI_KEY" > /tmp/peerTokNo.log 2>&1 & PB_PID=$!
sleep 3
kill -9 $PB_PID 2>/dev/null; PB_PID=""
grep -q "bad or expired connect token" /tmp/peerTokNo.log && ok "missing token rejected" || fail "missing token not rejected"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$CLI_UID" "$DEV_UID" -k "$CLI_KEY" -t "$EXPT" > /tmp/peerTokExp.log 2>&1 & PB_PID=$!
sleep 3
kill -9 $PB_PID 2>/dev/null; PB_PID=""
grep -q "bad or expired connect token" /tmp/peerTokExp.log && ok "expired token rejected" || fail "expired token not rejected"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$CLI_UID" "$DEV_UID" -k "$CLI_KEY" -t "$WRONGT" > /tmp/peerTokBad.log 2>&1 & PB_PID=$!
sleep 3
kill -9 $PB_PID 2>/dev/null; PB_PID=""
grep -q "bad or expired connect token" /tmp/peerTokBad.log && ok "wrong-src token rejected" || fail "wrong-src token not rejected"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$CLI_UID" "$DEV_UID" -k "$CLI_KEY" -t "$TOK" > /tmp/peerTokOk.log 2>&1 & PB_PID=$!
sleep 8
grep -q "CONNECTED to $DEV_UID via direct" /tmp/peerTokOk.log && ok "valid token CONNECT" || fail "valid token connect missing"
kill -9 $PB_PID 2>/dev/null; PB_PID=""
# 同一 Token 再连一次：nonce 已用，应被拒
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT "$CLI_UID" "$DEV_UID" -k "$CLI_KEY" -t "$TOK" > /tmp/peerTokReplay.log 2>&1 & PB_PID=$!
sleep 3
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "bad or expired connect token" /tmp/peerTokReplay.log && ok "replayed token rejected" || fail "replayed token not rejected"
grep -q "CONNECT token replay" /tmp/nat.log && ok "server logged token replay" || fail "server token replay log missing"
grep -q "CONNECT token rejected" /tmp/nat.log && ok "server logged token rejects" || fail "server token reject log missing"
stop_servers

# ---------------------------------------------------------------- 15. 局域网组播发现（LAN Search）
echo "== [15] LAN multicast discovery =="
start_servers ""
P2P_DISABLE_LAN=0 stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT LANDEV -lan > /tmp/peerLanDev.log 2>&1 & PA_PID=$!
sleep 1
P2P_DISABLE_LAN=0 stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT LANCLI LANDEV -lan > /tmp/peerLanCli.log 2>&1 & PB_PID=$!
sleep 6
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "LAN found LANDEV" /tmp/peerLanCli.log && ok "client found device on LAN" || fail "LAN search client ($([ -f /tmp/peerLanCli.log ] && cat /tmp/peerLanCli.log | tail -20))"
grep -q "LAN found LANCLI" /tmp/peerLanDev.log && ok "device found client on LAN" || fail "LAN search device"
grep -q "CONNECTED to LANDEV via direct" /tmp/peerLanCli.log && ok "LAN peers still P2P connect" || fail "LAN peers connect missing"
stop_servers

# ---------------------------------------------------------------- 16. ICE restart（换 ufrag 重建 agent）
echo "== [16] ICE restart =="
sleep 0.5
start_servers ""
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA16.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A -restart > /tmp/peerB16.log 2>&1 & PB_PID=$!
sleep 10
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "CONNECTED to A via direct" /tmp/peerB16.log && ok "B->A direct before restart" || fail "B->A direct missing ($([ -f /tmp/peerB16.log ] && tail -30 /tmp/peerB16.log))"
grep -q "ICE restart requested" /tmp/peerB16.log && ok "B requested ICE restart" || fail "ICE restart not requested"
grep -q "ICE restart offer" /tmp/peerB16.log && ok "B sent restart offer" || fail "ICE restart offer missing"
grep -q "ICE restart answer" /tmp/peerA16.log && ok "A applied restart answer" || fail "ICE restart answer missing"
grep -q "ICE restarted" /tmp/peerB16.log && ok "B ICE restarted" || fail "B ICE restarted missing"
stop_servers

# ---------------------------------------------------------------- 17. TURN-over-443（Proxy 兼听高位端口，对标 UDP/443）
echo "== [17] TURN-over-443 alt port =="
PROXY_ALT=18443
printf 'ProxyAltPort=%d\n' $PROXY_ALT > $CFG
NAT_PID=""
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 2 0 $PROXY_ALT > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5
grep -q "TURN-over-443 alt listen" /tmp/proxy.log && ok "proxy alt listen" || fail "proxy alt listen missing"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A 127.0.0.1 $PROXY_ALT -relay > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A 127.0.0.1 $PROXY_ALT -relay > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 7
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "CONNECTED to A via relay" /tmp/peerB.log && ok "B->A relay via alt port" || fail "alt-port relay missing"
grep -q "register ok" /tmp/proxy.log && ok "proxy register on alt" || fail "proxy register missing"
stop_all

# ---------------------------------------------------------------- 18. DERP TCP/TLS 强制中继
echo "== [18] DERP TCP/TLS force-relay =="
PROXY_TCP=18444
printf 'ProxyTcpPort=%d\n' $PROXY_TCP > $CFG
NAT_PID=""
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 2 0 0 $PROXY_TCP > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5
grep -q "DERP tcp listen" /tmp/proxy.log && ok "proxy derp tcp listen" || fail "derp tcp listen missing"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A 127.0.0.1 $PROXY_PORT -relay -tcp $PROXY_TCP -tls > /tmp/peerA18.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A 127.0.0.1 $PROXY_PORT -relay -tcp $PROXY_TCP -tls > /tmp/peerB18.log 2>&1 & PB_PID=$!
sleep 7
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "DERP tcp ready" /tmp/peerA18.log && ok "A derp tcp ready" || fail "A derp tcp ready missing"
grep -q "register ok tcp" /tmp/proxy.log && ok "proxy tcp register" || fail "proxy tcp register missing"
grep -q "CONNECTED to A via relay" /tmp/peerB18.log && ok "B->A via derp tcp" || fail "derp tcp relay missing"
stop_all

# ---------------------------------------------------------------- 19. DERP 先通再切直连（TCP 立即出图，ICE 并行升级）
echo "== [19] DERP first-then-upgrade =="
printf 'ProxyTcpPort=%d\n' $PROXY_TCP > $CFG
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 2 0 0 $PROXY_TCP > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A 127.0.0.1 $PROXY_PORT -tcp $PROXY_TCP -tls > /tmp/peerA19.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A 127.0.0.1 $PROXY_PORT -tcp $PROXY_TCP -tls > /tmp/peerB19.log 2>&1 & PB_PID=$!
sleep 8
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "DERP tcp registered" /tmp/peerA19.log && ok "A derp registered before punch" || fail "A derp register missing"
grep -q "CONNECTED to A via" /tmp/peerB19.log && ok "B connected (relay or direct)" || fail "B connect missing"
# 回环上 ICE 很快，允许只看到 direct；若先中继再直连则记一条升级
if grep -q "CONNECTED to A via relay" /tmp/peerB19.log && grep -q "CONNECTED to A via direct" /tmp/peerB19.log; then
    ok "B path upgrade relay -> direct"
elif grep -q "CONNECTED to A via direct" /tmp/peerB19.log; then
    ok "B ended on direct (ICE won race)"
else
    grep -q "CONNECTED to A via relay" /tmp/peerB19.log && ok "B stayed on derp relay" || fail "B no connected path"
fi
stop_all

# ---------------------------------------------------------------- 20. AltPort 兼听 TCP（不单独传 TcpPort）+ CONNECT 宣告
echo "== [20] AltPort dual-stack TCP (no explicit TcpPort) =="
PROXY_ALT=18445
printf 'ProxyAltPort=%d\n' $PROXY_ALT > $CFG
stdbuf -oL $NAT_BIN $NAT_PORT $PROXY_PORT 127.0.0.1 $CFG > /tmp/nat.log 2>&1 &
NAT_PID=$!
sleep 0.5
stdbuf -oL $PROXY_BIN $PROXY_PORT 100 2 0 $PROXY_ALT > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5
grep -q "DERP tcp listen $PROXY_ALT" /tmp/proxy.log && ok "alt port also listens TCP" || fail "alt dual tcp listen missing"
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A 127.0.0.1 $PROXY_PORT -relay > /tmp/peerA20.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A 127.0.0.1 $PROXY_PORT -relay > /tmp/peerB20.log 2>&1 & PB_PID=$!
sleep 8
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "DERP tcp ready" /tmp/peerB20.log && ok "B derp via CONNECT ProxyAltPort" || fail "B derp from CONNECT missing"
grep -q "register ok tcp" /tmp/proxy.log && ok "proxy tcp register on alt" || fail "alt tcp register missing"
grep -q "CONNECTED to A via relay" /tmp/peerB20.log && ok "B->A relay (alt dual)" || fail "alt dual relay missing"
stop_all

echo
echo "======================================"
echo "PASS=$PASS FAIL=$FAIL"
[ -n "$FAIL_MSG" ] && echo -e "failures:$FAIL_MSG"
echo "======================================"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
