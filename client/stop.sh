#!/bin/bash
# 停止本项目客户端 peer 进程
# 用法：
#   ./stop.sh              停止全部 peer
#   ./stop.sh device001    只停止指定 UUID 的 peer
cd "$(dirname "$0")"

UUID="$1"

if [ -n "$UUID" ]; then
    # 匹配绝对/相对路径启动的 peer，且参数中含该 UUID
    pids=$(ps -C peer -o pid=,args= 2>/dev/null | awk -v u="$UUID" '
        {
            for (i = 2; i <= NF; i++) {
                if ($i == u) { print $1; break }
            }
        }')
else
    pids=$(ps -C peer -o pid= 2>/dev/null)
fi

if [ -z "$pids" ]; then
    echo "[INFO] 没有找到运行中的 peer 客户端"
    exit 0
fi

echo "$pids" | xargs -r kill -9 2>/dev/null
count=$(echo "$pids" | wc -w | tr -d ' ')
if [ -n "$UUID" ]; then
    echo "[OK] 已停止 UUID=$UUID 的客户端 ($count 个)"
else
    echo "[OK] 已停止全部客户端 ($count 个)"
fi
exit 0
