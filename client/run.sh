#!/bin/bash
# P2P 客户端启动脚本
# 用法：
#   ./run.sh local <UUID> [对端UUID]          本地模式（连接 127.0.0.1）
#   ./run.sh public <UUID> [对端UUID]          公网模式（连接 43.136.55.143）
#   ./run.sh <IP> <UUID> [对端UUID]            自定义服务器 IP
#
# 可选参数：
#   -s <secret>    鉴权密钥
#   -relay         强制中继模式
cd "$(dirname "$0")"

NAT_PORT=16001
PROXY_PORT=16002
PUBLIC_IP="43.136.55.143"

# 解析模式参数
MODE="$1"
case "$MODE" in
    local)
        SERVER_IP="127.0.0.1"
        shift
        ;;
    public)
        SERVER_IP="$PUBLIC_IP"
        shift
        ;;
    *)
        if [[ "$MODE" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
            SERVER_IP="$MODE"
            shift
        else
            echo "用法:"
            echo "  $0 local <UUID> [对端UUID] [-s secret] [-relay]"
            echo "  $0 public <UUID> [对端UUID] [-s secret] [-relay]"
            echo "  $0 <IP> <UUID> [对端UUID] [-s secret] [-relay]"
            echo ""
            echo "示例:"
            echo "  $0 local device001                          # 本地等待方"
            echo "  $0 local device002 device001                # 本地发起方"
            echo "  $0 public device001 -s s3cr3t              # 公网带鉴权"
            echo "  $0 public device002 device001 -relay        # 公网强制中继"
            exit 1
        fi
        ;;
esac

# 剩余参数：UUID [对端UUID] [-s secret] [-relay]
UUID="$1"
shift

if [ -z "$UUID" ]; then
    echo "错误：缺少 UUID 参数"
    exit 1
fi

# 确保已编译
if [ ! -f bin/peer ]; then
    echo "[INFO] 编译客户端..."
    make -s 2>&1
    if [ ! -f bin/peer ]; then
        echo "[ERROR] 编译失败，请检查依赖"
        exit 1
    fi
fi

# 构建命令行
CMD="./bin/peer $SERVER_IP $NAT_PORT $UUID"

# 附加剩余参数（对端UUID / -s / -relay）
if [ $# -gt 0 ]; then
    CMD="$CMD $*"
fi

# 如果有对端 UUID 且不是 - 开头的参数，追加 proxy 地址
PEER_UUID=""
for arg in "$@"; do
    if [[ "$arg" != -* ]]; then
        PEER_UUID="$arg"
        break
    fi
done

if [ -n "$PEER_UUID" ]; then
    CMD="$CMD $SERVER_IP $PROXY_PORT"
fi

echo "========================================"
echo " P2P 客户端启动"
echo "  服务器:   $SERVER_IP:$NAT_PORT"
echo "  本机UUID: $UUID"
if [ -n "$PEER_UUID" ]; then
    echo "  对端UUID: $PEER_UUID"
fi
echo "  命令:    $CMD"
echo "========================================"

exec $CMD
