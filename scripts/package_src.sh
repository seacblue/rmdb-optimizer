#!/bin/bash
# ============================================================
# package_src.sh — 从项目根目录打包可独立编译的生产子集
#
# 用法:
#   ./scripts/package_src.sh                          # 默认 → rmdb-src.zip
#   ./scripts/package_src.sh  /path/to/output.zip     # 指定路径
#   ./scripts/package_src.sh  -o my_src.zip           # 指定文件名
#   ZIP=submit.zip ./scripts/package_src.sh           # 环境变量
#
# 包含:
#   - CMakeLists.txt (根 + 所有子目录)
#   - cmake/         (CMake 模块)
#   - deps/          (googletest 依赖)
#   - scripts/       (构建脚本)
#   - src/           (生产代码，不含测试)
#
# 排除:
#   - src/test/      (全部测试)
#   - docs/          (文档)
#   - build/         (编译产物)
#   - .git/          (版本历史)
#   - .github/       (CI 配置)
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 解析输出路径
if [[ $# -gt 0 ]]; then
    if [[ "$1" == "-o" && -n "${2:-}" ]]; then
        if [[ "$2" = /* ]]; then
            OUTPUT_ZIP="$2"
        else
            OUTPUT_ZIP="${PROJECT_DIR}/$2"
        fi
    else
        OUTPUT_ZIP="$1"
    fi
elif [[ -n "${ZIP:-}" ]]; then
    if [[ "$ZIP" = /* ]]; then
        OUTPUT_ZIP="$ZIP"
    else
        OUTPUT_ZIP="${PROJECT_DIR}/$ZIP"
    fi
else
    OUTPUT_ZIP="${PROJECT_DIR}/rmdb-src.zip"
fi

mkdir -p "$(dirname "$OUTPUT_ZIP")"
cd "$PROJECT_DIR"

# 构建排除参数
EXCLUDES=(
    "src/test/*"                        # 全部测试
    "docs/*"                            # 文档
    "build/*"                           # 编译产物
    ".git/*"                            # 版本历史
    ".github/*"                         # CI 配置
)

ZIP_OPTS=()
for pattern in "${EXCLUDES[@]}"; do
    ZIP_OPTS+=(-x "$pattern")
done

# 打包：从根目录递归，只包含构建所需文件
zip -qr "$OUTPUT_ZIP" \
    CMakeLists.txt \
    cmake/ \
    deps/ \
    scripts/ \
    src/ \
    "${ZIP_OPTS[@]}"

echo "打包完成: ${OUTPUT_ZIP}"
echo "大小: $(stat --printf='%s' "$OUTPUT_ZIP" 2>/dev/null | numfmt --to=iec 2>/dev/null || ls -lh "$OUTPUT_ZIP" | awk '{print $5}')"
