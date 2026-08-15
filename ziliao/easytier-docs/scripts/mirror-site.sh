#!/usr/bin/env bash
# 镜像 https://www.easytier.cn/ 静态文档站（VitePress）
# 用法：./mirror-site.sh [输出目录]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${1:-${SCRIPT_DIR}/../site-mirror}"
URL_FILE="${SCRIPT_DIR}/urls.txt"
mkdir -p "$OUT_DIR"

# 先按已知页面清单抓取，再从首页递归补漏
wget \
  --input-file="$URL_FILE" \
  --page-requisites \
  --convert-links \
  --adjust-extension \
  --no-parent \
  --domains=www.easytier.cn,easytier.cn \
  --span-hosts \
  --restrict-file-names=windows \
  --directory-prefix="$OUT_DIR" \
  --no-host-directories \
  --reject-regex='[?].*' \
  --timeout=20 \
  --tries=2 \
  --wait=0.1 \
  --random-wait \
  --execute robots=off \
  --user-agent='Mozilla/5.0 (compatible; EasyTierDocsArchive/1.0)' \
  || true

wget \
  --recursive \
  --level=4 \
  --page-requisites \
  --convert-links \
  --adjust-extension \
  --no-parent \
  --domains=www.easytier.cn,easytier.cn \
  --restrict-file-names=windows \
  --directory-prefix="$OUT_DIR" \
  --no-host-directories \
  --reject-regex='[?].*' \
  --timeout=20 \
  --tries=2 \
  --wait=0.1 \
  --random-wait \
  --execute robots=off \
  --user-agent='Mozilla/5.0 (compatible; EasyTierDocsArchive/1.0)' \
  https://www.easytier.cn/ \
  https://www.easytier.cn/en/ \
  || true

echo "镜像完成: $OUT_DIR"
find "$OUT_DIR" -type f | wc -l
du -sh "$OUT_DIR"
