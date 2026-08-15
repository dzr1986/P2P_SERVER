#!/bin/bash
# NAT 组合矩阵：打印 RFC 4787 二维 × 两端的打洞策略。
# CI 无 NET_ADMIN 时只打印表（默认）。现网/实验室可加 --iptables-demo 看规则样例。
# 对照 DCUtR：锥型组合走 ICE；EDM×EDM 必须中继；NAT4E 优先预测。
set -euo pipefail

birthday=0
demo=0
for a in "$@"; do
    case "$a" in
        --birthday) birthday=1 ;;
        --iptables-demo) demo=1 ;;
        -h|--help)
            echo "usage: $0 [--birthday] [--iptables-demo]"
            exit 0
            ;;
    esac
done

echo "== RFC 4787 NAT matrix (birthday=$birthday) =="
printf '%-8s %-8s %-10s %s\n' "A" "B" "step" "strategy"
for a in EIM EDM; do
    for b in EIM EDM; do
        sa=0; sb=0
        [ "$a" = EDM ] && sa=1
        [ "$b" = EDM ] && sb=1
        if [ "$a" = EDM ] && [ "$b" = EDM ]; then
            if [ "$birthday" = 1 ]; then s=birthday; else s=relay; fi
        elif [ "$a" = EDM ] && [ "$sa" != 0 ] && [ "$b" = EIM ]; then
            s=predict
        elif [ "$b" = EDM ] && [ "$sb" != 0 ] && [ "$a" = EIM ]; then
            s=predict
        else
            s=ice
        fi
        printf '%-8s %-8s %-10s %s\n' "$a" "$b" "${sa}/${sb}" "$s"
    done
done
echo
echo "notes:"
echo "  - EIM×EIM / 锥型：标准 ICE（本仓库 juice）；用户态对照 tests/bin/nat_sim_test"
echo "  - EIM×EDM 且识别 NAT4E step：端口预测，不先扫"
echo "  - EDM×EDM：必须中继；--birthday 才允许生日扫描（默认关）"
echo "  - filter=none|addr|port 由 ICE 连通性检查覆盖，不单独改策略"
echo "  - 对照 DCUtR：分类型统计，对称对不计入锥型 ≥85% 分母"
echo "  - 本环境/CI 无 iptables 与 netns：用 NatSim 盒模拟 STUN 打洞，不假装现网矩阵已通"

if [ "$demo" = 1 ]; then
    echo
    echo "== iptables demo (needs NET_ADMIN; not run by CI) =="
    cat <<'RULES'
# 全锥（EIM + filter=none）：SNAT 固定，不按来源过滤
#   iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -j SNAT --to-source $WAN
# 端口受限（EIM + filter=port）：conntrack 默认（仅已发出五元组可回）
#   iptables -t nat -A POSTROUTING -s 10.8.1.0/24 -j MASQUERADE
# 对称 / EDM：每目的换源端口（nf_nat 默认 MASQUERADE 近似）
#   iptables -t nat -A POSTROUTING -s 10.8.2.0/24 -p udp -j MASQUERADE --random
# docker 两侧各挂一种 NAT 网桥后跑 peer A/B，对照 punch_strategy()。
RULES
fi
