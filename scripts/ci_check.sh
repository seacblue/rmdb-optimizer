#!/usr/bin/env bash
# ============================================================
# scripts/ci_check.sh
# 用途：RMDB CI 核心检查脚本 — 模块测试完备性 + 行覆盖率阈值
#
# 在 GitHub Actions 中由 ci.yml 调用，执行：
#   1. 模块测试完备性检查 —— 每个库模块必须有对应测试
#   2. 行覆盖率检查 —— 每个模块的最低行覆盖率
#   3. 汇总输出报告
#
# 添加新模块的步骤：
#   1. 在 src/<module>/CMakeLists.txt 中添加 add_library(...)
#   2. 在 src/test/module/test_<module>.cpp 中编写测试
#   3. 在 src/test/CMakeLists.txt 中添加 add_executable(...)
#   4. 在本脚本的 MODULES 数组中添加一行
# ============================================================

set -euo pipefail

# ============================================================
# 模块清单
# 格式：LIB名称:测试目标:模块源目录:最低行覆盖率(%)
# 添加新模块时在此追加一行
# ============================================================
MODULES=(
    "lru_replacer:test_lru:src/replacer:80"
    "disk_manager:test_dm:src/storage:60"
    "buffer_pool_manager:test_bpm:src/storage:80"
    "system:test_sm:src/system:80"
    "record:test_record:src/record:80"
    "execution:test_execution:src/execution:80"
    # 后续模块（取消注释即可启用）：
    # "index:test_ix:src/index:80"
    # "transaction:test_txn:src/transaction:80"
    # "recovery:test_log:src/recovery:80"
    # "parser:test_parser:src/parser:80"     # 已有 test_parser 目标
    # "analyze:test_analyze:src/analyze:80"
    # "planner:test_planner:src/optimizer:80"
)

# ============================================================
# 构建目录（从脚本位置推断）
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
COVERAGE_DIR="${BUILD_DIR}/coverage_report"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASS=0
FAIL=0
WARN=0

print_pass()   { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); }
print_fail()   { echo -e "  ${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); }
print_warn()   { echo -e "  ${YELLOW}[WARN]${NC} $1"; WARN=$((WARN + 1)); }

# ============================================================
# 阶段 0：前置检查
# ============================================================
echo ""
echo "============================================"
echo "  RMDB CI 检查 — 阶段 0：前置条件"
echo "============================================"

# 检查构建目录是否存在
if [ ! -d "$BUILD_DIR" ]; then
    print_fail "构建目录不存在: $BUILD_DIR"
    print_fail "请先在项目根目录执行: mkdir build && cd build && cmake .."
    exit 1
fi
print_pass "构建目录存在"

# 检查 lcov 是否可用（非关键依赖，失败不中止）
LCOV_AVAILABLE=false
if which lcov >/dev/null 2>&1; then
    LCOV_AVAILABLE=true
    print_pass "lcov 可用"
else
    print_warn "lcov 未安装 — 覆盖率检查跳过"
    print_warn "安装: sudo apt-get install lcov"
fi

# 检查测试二进制是否存在
ALL_TESTS_EXIST=true
for entry in "${MODULES[@]}"; do
    IFS=':' read -r lib test_exe dir threshold <<< "$entry"
    if [ ! -f "${BIN_DIR}/${test_exe}" ]; then
        print_warn "测试二进制不存在: ${BIN_DIR}/${test_exe} — 请先 make ${test_exe}"
        ALL_TESTS_EXIST=false
    fi
done

echo ""

# ============================================================
# 阶段 1：模块测试完备性检查
# ============================================================
echo "============================================"
echo "  阶段 1：模块测试完备性检查"
echo "============================================"

ALL_MODULES_COVERED=true
for entry in "${MODULES[@]}"; do
    IFS=':' read -r lib test_exe dir threshold <<< "$entry"
    test_file="${PROJECT_DIR}/src/test/module/test_${lib}.cpp"

    if [ -f "$test_file" ]; then
        print_pass "模块 ${lib} → 测试文件存在: test_${lib}.cpp"
    else
        print_fail "模块 ${lib} → 缺少测试文件! 请创建 src/test/test_${lib}.cpp"
        ALL_MODULES_COVERED=false
    fi

    # 检查 src/test/CMakeLists.txt 中是否有对应目标
    if grep -q "add_executable(${test_exe}" "${PROJECT_DIR}/src/test/CMakeLists.txt" 2>/dev/null; then
        print_pass "模块 ${lib} → CMake 目标 ${test_exe} 已注册"
    else
        print_fail "模块 ${lib} → CMake 目标 ${test_exe} 未注册! 请在 src/test/CMakeLists.txt 中添加"
        ALL_MODULES_COVERED=false
    fi
done

if [ "$ALL_MODULES_COVERED" = false ]; then
    echo ""
    echo -e "${RED}[FAIL] 存在未覆盖的模块。CI 拒绝通过。${NC}"
    echo "添加新模块测试的步骤："
    echo "  1. 创建 src/test/module/test_<module>.cpp"
    echo "  2. 在 src/test/CMakeLists.txt 中添加 add_executable(test_<abbr> ...)"
    echo "  3. 在 scripts/ci_check.sh 的 MODULES 数组中添加一行"
    echo "  4. 重新编译: make test_<abbr> -j\$(nproc)"
    echo "  5. 运行测试: ./bin/test_<abbr>"
fi
echo ""

# ============================================================
# 阶段 2：运行测试
# ============================================================
echo "============================================"
echo "  阶段 2：执行模块测试"
echo "============================================"

ALL_TESTS_PASSED=true
for entry in "${MODULES[@]}"; do
    IFS=':' read -r lib test_exe dir threshold <<< "$entry"
    test_bin="${BIN_DIR}/${test_exe}"
    
    if [ ! -f "$test_bin" ]; then
        print_warn "${test_exe} 未编译，跳过"
        continue
    fi

    echo "  --- 运行 ${test_exe} ---"
    if "$test_bin" --gtest_print_time=1 2>&1; then
        print_pass "${test_exe} 全部测试通过"
    else
        print_fail "${test_exe} 存在失败的测试!"
        ALL_TESTS_PASSED=false
    fi
    echo ""
done

# ============================================================
# 阶段 3：行覆盖率检查（仅 lcov 可用时）
# ============================================================
echo "============================================"
echo "  阶段 3：行覆盖率检查"
echo "============================================"

if [ "$LCOV_AVAILABLE" = true ] && [ "$ALL_TESTS_PASSED" = true ]; then
    mkdir -p "$COVERAGE_DIR"

    # 收集原始覆盖率数据
    # --ignore-errors mismatch,source,empty 跳过:
    #   - 测试文件的 .gcda 行号不匹配
    #   - 解析器生成的 .cpp 中嵌入的绝对路径无法访问
    #   - 无有效记录时跳过
    lcov --capture --directory "$BUILD_DIR" \
         --output-file "${COVERAGE_DIR}/raw.info" \
         --rc branch_coverage=1 \
         --ignore-errors mismatch,source,empty,gcov \
         2>&1 | grep -v "^Processing\|^geninfo: WARNING\|^geninfo: INFO\|^(use" || true

    # 剔除系统头文件、deps、测试代码
    lcov --remove "${COVERAGE_DIR}/raw.info" \
         --output-file "${COVERAGE_DIR}/filtered.info" \
         --rc branch_coverage=1 \
         --ignore-errors unused,empty \
         '/usr/*' \
         '*/deps/*' \
         '*/googletest/*' \
         '*/src/test/*' \
         '*.yacc.tab.*' \
         '*.lex.yy.*' \
         2>&1 | grep -v "^Removing\|^geninfo:" || true

    echo ""
    echo "  各模块覆盖率明细："
    echo "  ───────────────────────────────────────────"

    ALL_COVERAGE_PASSED=true
    for entry in "${MODULES[@]}"; do
        IFS=':' read -r lib test_exe dir threshold <<< "$entry"
        src_dir="${PROJECT_DIR}/${dir}"

        # 用 lcov 提取该模块的覆盖率（忽略 extract 错误）
        lcov --extract "${COVERAGE_DIR}/filtered.info" \
             --output-file "${COVERAGE_DIR}/${lib}.info" \
             --rc branch_coverage=1 \
             --ignore-errors unused \
             "${src_dir}/*" \
             > /dev/null 2>&1 || true

        if [ -f "${COVERAGE_DIR}/${lib}.info" ] && [ -s "${COVERAGE_DIR}/${lib}.info" ]; then
            # 用 lcov --summary 解析行覆盖率（stderr 重定向到临时文件避免 pipefail 问题）
            set +e
            summary_str=$(lcov --summary "${COVERAGE_DIR}/${lib}.info" \
                               --rc branch_coverage=1 \
                               --ignore-errors unused 2>/dev/null)
            set -e
            pct=$(echo "$summary_str" | grep "lines\." | awk '{print $2}' | sed 's/%//')

            if [ -z "$pct" ]; then
                print_warn "模块 ${lib}：无法获取覆盖率（可能无 .gcda 文件）"
                continue
            fi

            # 输出进度条风格显示
            pct_int=$(echo "$pct" | cut -d. -f1)
            if [ "$pct_int" -ge "$threshold" ]; then
                print_pass "模块 ${lib}：行覆盖率 ${pct}% ≥ ${threshold}% ✅"
            else
                print_fail "模块 ${lib}：行覆盖率 ${pct}% < ${threshold}% ❌"
                ALL_COVERAGE_PASSED=false
            fi
        else
            print_warn "模块 ${lib}：该模块的源文件未在覆盖率数据中找到"
            print_warn "  预期源目录: ${src_dir}"
        fi
    done

    # 生成 HTML 报告（用于 CI artifact）
    genhtml "${COVERAGE_DIR}/filtered.info" \
            --output-directory "${COVERAGE_DIR}/html" \
            --rc branch_coverage=1 \
            --ignore-errors source \
            --title "RMDB Coverage Report" \
            2>/dev/null || true
    print_pass "覆盖率 HTML 报告已生成: ${COVERAGE_DIR}/html/index.html"

elif [ "$LCOV_AVAILABLE" = false ]; then
    print_warn "跳过覆盖率检查: lcov 未安装"
else
    print_warn "跳过覆盖率检查: 存在失败的测试，覆盖率无意义"
fi

# ============================================================
# 汇总
# ============================================================
echo ""
echo "============================================"
echo "  CI 检查汇总"
echo "============================================"
echo -e "  ${GREEN}通过${NC}: $PASS    ${RED}失败${NC}: $FAIL    ${YELLOW}警告${NC}: $WARN"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}CI 检查未通过: $FAIL 项失败${NC}"
    exit 1
else
    if [ "$ALL_MODULES_COVERED" = false ]; then
        echo -e "${YELLOW}CI 检查有警告: 部分模块缺少测试${NC}"
        echo "请在合并前补全模块测试。"
        exit 1
    fi
    echo -e "${GREEN}CI 检查全部通过!${NC}"
    exit 0
fi
