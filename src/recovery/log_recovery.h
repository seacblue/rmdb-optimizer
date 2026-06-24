/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <map>
#include <unordered_map>
#include <vector>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RedoLogsInPage {
public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle* table_file_;
    std::vector<lsn_t> redo_logs_;   // 在该page上需要redo的操作的lsn
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
        log_manager_ = nullptr;
    }

    ~RecoveryManager() {
        for (auto* rec : log_records_) {
            delete rec;
        }
        log_records_.clear();
    }

    void set_log_manager(LogManager* log_manager) { log_manager_ = log_manager; }

    void analyze();
    void redo();
    void undo();

private:
    // 从磁盘读取所有日志并解析成日志记录对象
    void parse_logs();
    // 根据表名获取记录文件句柄
    RmFileHandle* get_file_handle(const std::string& tab_name);
    // 重建所有表的索引（恢复结束后调用，保证索引与堆一致）
    void rebuild_all_indexes();

    LogBuffer buffer_;                                              // 读入日志
    DiskManager* disk_manager_;                                     // 用来读写文件
    BufferPoolManager* buffer_pool_manager_;                        // 对页面进行读写
    SmManager* sm_manager_;                                         // 访问数据库元数据
    LogManager* log_manager_;                                       // 用于恢复后续写global_lsn

    std::vector<LogRecord*> log_records_;                          // 按lsn顺序保存的所有日志记录
    std::unordered_map<lsn_t, size_t> lsn_to_idx_;                 // lsn -> log_records_下标
    std::unordered_map<txn_id_t, lsn_t> att_;                     // 活跃事务表: txn_id -> 最后一条日志lsn
    lsn_t max_lsn_{INVALID_LSN};                                   // 日志中出现的最大lsn
};