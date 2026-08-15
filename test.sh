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
CFG=/tmp/p2p_auth_test.cfg

PASS=0
FAIL=0
FAIL_MSG=""

fail() { FAIL=$((FAIL+1)); FAIL_MSG="$FAIL_MSG\n  $1"; }
ok()   { PASS=$((PASS+1)); echo "  ok: $1"; }

# 统一清理
cleanup() {
    for p in $NAT_PID $NAT2_PID $PROXY_PID $PA_PID $PB_PID $IOTC_DEV_PID; do
        [ -n "$p" ] && kill -9 $p 2>/dev/null
    done
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

# ---------------------------------------------------------------- 0. 单元测试
echo "== [0] unit tests =="
./tests/bin/crypto_test > /tmp/crypto_test.log 2>&1 && ok "crypto unit tests" || fail "crypto unit tests"
./tests/bin/session_test > /tmp/session_test.log 2>&1 && ok "session unit tests" || fail "session unit tests"
./tests/bin/uid_test > /tmp/uid_test.log 2>&1 && ok "uid unit tests" || fail "uid unit tests"
./tests/bin/av_frame_test > /tmp/av_frame_test.log 2>&1 && ok "av/tunnel codec unit tests" || fail "av/tunnel codec unit tests"

# ---------------------------------------------------------------- 1. 直连
echo "== [1] direct P2P (no auth) =="
start_servers ""
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT B A > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 6
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
sleep 7
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

stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT A > /tmp/peerA.log 2>&1 & PA_PID=$!
sleep 1
stdbuf -oL $PEER_BIN 127.0.0.1 $NAT_PORT2 B A 127.0.0.1 $PROXY_PORT > /tmp/peerB.log 2>&1 & PB_PID=$!
sleep 7
kill -9 $PA_PID $PB_PID 2>/dev/null; PA_PID=""; PB_PID=""
grep -q "sync entry uuid\[A\]" /tmp/nat2.log && ok "signed sync entry accepted" || fail "signed sync not accepted"
grep -q "CONNECTED to A via direct" /tmp/peerB.log && ok "B->A direct (signed sync)" || fail "cross-server direct missing"
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

echo
echo "======================================"
echo "PASS=$PASS FAIL=$FAIL"
[ -n "$FAIL_MSG" ] && echo -e "failures:$FAIL_MSG"
echo "======================================"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
