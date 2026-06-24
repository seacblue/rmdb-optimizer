#!/bin/bash
# dual_build.sh
# 编译 RMDB 服务器/客户端，并启动双窗口
#
# 用法:
#   bash scripts/dual_build.sh                    # 编译 + 启动双窗口
#   bash scripts/dual_build.sh mydb              # 指定数据库名
#   bash scripts/dual_build.sh --no-launch       # 只编译，不启动窗口
#   bash scripts/dual_build.sh --server-only     # 只编译 server
#   bash scripts/dual_build.sh --client-only     # 只编译 client
#   bash scripts/dual_build.sh --launch-only     # 不编译，只启动窗口

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER_DIR="$ROOT/build"
CLIENT_DIR="$ROOT/rmdb_client/build"
DB_PATH="${DB_PATH:-rmdb}"

build_server() {
    mkdir -p "$SERVER_DIR"
    cd "$SERVER_DIR"
    # 如果 build 目录没有 Makefile，先 cmake 配置
    if [ ! -f Makefile ]; then
        cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -3
    fi
    make -j "$(nproc)" 2>&1 | tail -5
}

build_client() {
    mkdir -p "$CLIENT_DIR"
    cd "$CLIENT_DIR"
    # 如果 client build 目录没有 Makefile，先 cmake 配置
    if [ ! -f Makefile ]; then
        cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -3
    fi
    make -j "$(nproc)" 2>&1 | tail -3
}

launch_windows() {
    local db="${1:-rmdb}"
    local srv_script="$ROOT/scripts/run_server.sh"
    local cli_script="$ROOT/scripts/run_client.sh"

    # 直接用 wsl /path/to/script.sh 启动，避免多层引号解析问题
    if command -v wt.exe &>/dev/null; then
        wt.exe new-tab --title "RMDB Server (${db})" \
            wsl "$srv_script" "$db" &
        sleep 0.5
        wt.exe new-tab --title "RMDB Client" \
            wsl "$cli_script" &
    elif command -v cmd.exe &>/dev/null; then
        cmd.exe /c start "RMDB Server (${db})" \
            wsl "$srv_script" "$db"
        sleep 0.3
        cmd.exe /c start "RMDB Client" \
            wsl "$cli_script"
    else
        local SESSION="rmdb_session"
        tmux kill-session -t "$SESSION" 2>/dev/null
        tmux new-session -d -s "$SESSION" -n "rmdb" -c "$ROOT" \
            "$srv_script" "$db"
        sleep 0.3
        tmux split-window -h -c "$ROOT" -t "$SESSION:0" \
            "$cli_script"
        tmux select-layout -t "$SESSION:0" even-horizontal
        tmux select-pane -t "$SESSION:0.0"
        tmux attach-session -t "$SESSION"
    fi
}

print_help() {
    echo "Usage: bash scripts/dual_build.sh [OPTION] [db_name]"
    echo ""
    echo "Options:"
    echo "  (无参数)             编译 + 启动双窗口 (server + client)"
    echo "  --no-launch          只编译，不启动窗口"
    echo "  --server-only        只编译 server"
    echo "  --client-only        只编译 client"
    echo "  --launch-only        不编译，只启动双窗口"
    echo ""
    echo "环境变量: DB_PATH 可替代 <db> 参数"
}

# ===== 参数解析 =====
MODE="both"
DO_BUILD=true
DO_LAUNCH=true

case "${1:-}" in
    --no-launch)
        MODE="both"
        DO_LAUNCH=false
        DB_PATH="${2:-$DB_PATH}"
        ;;
    --server-only|server)
        MODE="server"
        DO_LAUNCH=false
        DB_PATH="${2:-$DB_PATH}"
        ;;
    --client-only|client)
        MODE="client"
        DO_LAUNCH=false
        DB_PATH="${2:-$DB_PATH}"
        ;;
    --launch-only)
        DO_BUILD=false
        DO_LAUNCH=true
        DB_PATH="${2:-$DB_PATH}"
        ;;
    --help|-h)
        print_help; exit 0
        ;;
    *)
        # 第一个参数是数据库名（可选），默认 rmdb
        DB_PATH="${1:-rmdb}"
        ;;
esac

# ===== 执行构建 =====
if $DO_BUILD; then
    case "$MODE" in
        both)
            build_server &
            build_client &
            wait
            ;;
        server)
            build_server
            ;;
        client)
            build_client
            ;;
    esac
fi

if $DO_LAUNCH; then
    launch_windows "$DB_PATH"
fi
