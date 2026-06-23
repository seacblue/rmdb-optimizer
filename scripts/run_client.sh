#!/bin/bash
# 启动 RMDB client，退出后保持窗口不关
cd "$(cd "$(dirname "$0")/.." && pwd)"
./rmdb_client/build/rmdb_client
exec bash
