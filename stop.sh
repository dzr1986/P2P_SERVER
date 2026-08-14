#!/bin/bash
# 停止本实现启动的 NatServer + P2PProxy（只匹配本项目二进制，避免误杀原版 8832/8833）
cd "$(dirname "$0")"
pkill -9 -f "server/natserver/bin/[p]2p_natserver" 2>/dev/null
pkill -9 -f "server/proxyserver/bin/[p]2p_proxy" 2>/dev/null
exit 0
