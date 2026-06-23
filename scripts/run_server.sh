#!/bin/bash
# 启动 RMDB server，退出后保持窗口不关
# 切换到 build/ 目录，使 db 目录创建在 build/<db_name>/ 下
cd "$(cd "$(dirname "$0")/.." && pwd)/build"
./bin/rmdb "${1:-rmdb}"
exec bash
