#!/usr/bin/env bash
# ============================================================
# scripts/ci_check.sh
# 轻量 CI 一致性检查：
#   1. rmdb 可执行文件已生成
#   2. 集成测试脚本存在
#   3. 集成测试 output.txt 已产出
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_PATH="${BUILD_DIR}/bin/rmdb"
INTEGRATION_SCRIPT="${PROJECT_DIR}/src/test/integration_test.py"
OUTPUT_PATH="${BUILD_DIR}/execution_test_db/output.txt"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; exit 1; }

echo ""
echo "============================================"
echo "  RMDB CI 轻量检查"
echo "============================================"

[ -d "$BUILD_DIR" ] || fail "构建目录不存在: $BUILD_DIR"
pass "构建目录存在"

[ -x "$BIN_PATH" ] || fail "rmdb 可执行文件不存在: $BIN_PATH"
pass "rmdb 可执行文件存在"

[ -f "$INTEGRATION_SCRIPT" ] || fail "集成测试脚本不存在: $INTEGRATION_SCRIPT"
pass "集成测试脚本存在"

[ -f "$OUTPUT_PATH" ] || fail "未找到集成测试输出文件: $OUTPUT_PATH"
pass "集成测试输出文件存在"

echo ""
echo -e "${GREEN}CI 轻量检查通过${NC}"
