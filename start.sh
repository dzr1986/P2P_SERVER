#!/bin/bash
# 启动 NatServer + P2PProxy
# 用法：
#   ./start.sh           本地模式（绑定 127.0.0.1，仅本机测试）
#   ./start.sh public    公网模式（绑定 43.136.55.143，允许外部连接）
#   ./start.sh <IP>      自定义 IP（绑定指定地址）
cd "$(dirname "$0")"

NAT_PORT=16001
PROXY_PORT=16002

# 默认本地模式
BIND_IP="127.0.0.1"

# 解析参数
if [ "$1" = "public" ]; then
    BIND_IP="43.136.55.143"
elif [ -n "$1" ]; then
    BIND_IP="$1"
fi

echo "========================================"
echo " P2P 服务器启动"
echo "  模式: $BIND_IP"
echo "  NatServer:  $BIND_IP:$NAT_PORT (UDP)"
echo "  P2PProxy:  $BIND_IP:$PROXY_PORT (UDP)"
echo "========================================"

# 先停掉旧进程
pkill -9 -f "server/natserver/bin/[p]2p_natserver" 2>/dev/null
pkill -9 -f "server/proxyserver/bin/[p]2p_proxy" 2>/dev/null
sleep 1

# 启动 NatServer
./server/natserver/bin/p2p_natserver $NAT_PORT $PROXY_PORT $BIND_IP &
NAT_PID=$!
echo "[OK] NatServer started (PID=$NAT_PID)"

sleep 2

# 启动 P2PProxy
./server/proxyserver/bin/p2p_proxy $PROXY_PORT 100 &
PROXY_PID=$!
echo "[OK] P2PProxy started (PID=$PROXY_PID)"

echo ""
echo "----------------------------------------"
echo " 服务进程:"
echo "  NatServer  PID=$NAT_PID"
echo "  P2PProxy   PID=$PROXY_PID"
echo "----------------------------------------"
echo ""
if [ "$BIND_IP" = "127.0.0.1" ]; then
    echo " 本地测试客户端命令:"
    echo "  cd client && make"
    echo "  终端A: ./bin/peer 127.0.0.1 $NAT_PORT device001"
    echo "  终端B: ./bin/peer 127.0.0.1 $NAT_PORT device002 device001"
else
    echo " 公网测试客户端命令:"
    echo "  cd client && make"
    echo "  终端A: ./bin/peer $BIND_IP $NAT_PORT device001"
    echo "  终端B: ./bin/peer $BIND_IP $NAT_PORT device002 device001"
fi
echo ""
echo " 停止服务: ./stop.sh"
echo "========================================"
