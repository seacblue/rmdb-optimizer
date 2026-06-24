/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <algorithm>
#include <string>
#include <vector>

RmFileHandle* RecoveryManager::get_file_handle(const std::string& tab_name) {
    auto it = sm_manager_->fhs_.find(tab_name);
    if (it == sm_manager_->fhs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

// PLACEHOLDER_PARSE

// 从磁盘读取整个日志文件，按顺序反序列化为日志记录对象
void RecoveryManager::parse_logs() {
    int file_offset = 0;
    char header[LOG_HEADER_SIZE];

    while (true) {
        // 先读日志头，确定该条日志的总长度
        int read_bytes = disk_manager_->read_log(header, LOG_HEADER_SIZE, file_offset);
        if (read_bytes != LOG_HEADER_SIZE) {
            break;  // 文件结束或残缺日志
        }
        LogType log_type = *reinterpret_cast<const LogType*>(header + OFFSET_LOG_TYPE);
        uint32_t log_tot_len = *reinterpret_cast<const uint32_t*>(header + OFFSET_LOG_TOT_LEN);
        if (log_tot_len < static_cast<uint32_t>(LOG_HEADER_SIZE)) {
            break;  // 非法长度，停止解析
        }

        // 读取完整日志记录（使用 thread_local 缓冲区，避免每次堆分配）
        static thread_local std::string buf;
        buf.resize(log_tot_len);
        int got = disk_manager_->read_log(buf.data(), log_tot_len, file_offset);
        if (got != static_cast<int>(log_tot_len)) {
            break;  // 残缺日志，丢弃
        }

        LogRecord* rec = nullptr;
        switch (log_type) {
            case LogType::begin:   rec = new BeginLogRecord();  break;
            case LogType::commit:  rec = new CommitLogRecord(); break;
            case LogType::ABORT:   rec = new AbortLogRecord();  break;
            case LogType::INSERT:  rec = new InsertLogRecord(); break;
            case LogType::DELETE:  rec = new DeleteLogRecord(); break;
            case LogType::UPDATE:  rec = new UpdateLogRecord(); break;
            default:
                rec = nullptr;
                break;
        }
        if (rec == nullptr) {
            break;
        }
        rec->deserialize(buf.data());

        lsn_to_idx_[rec->lsn_] = log_records_.size();
        log_records_.push_back(rec);
        max_lsn_ = std::max(max_lsn_, rec->lsn_);

        file_offset += log_tot_len;
    }
}

// PLACEHOLDER_ANALYZE

/**
 * @description: analyze阶段，解析日志并构建未完成的事务列表（ATT）
 * 本题不含检查点，从第一条日志开始扫描；脏页表退化为对所有页面按page_lsn判断是否redo。
 */
void RecoveryManager::analyze() {
    parse_logs();

    if (log_manager_ != nullptr && max_lsn_ != INVALID_LSN) {
        log_manager_->set_global_lsn(max_lsn_ + 1);
    }

    // 顺序扫描日志，维护活跃事务表：遇到begin/写操作记录最后lsn，遇到commit/abort移除
    for (auto* rec : log_records_) {
        txn_id_t tid = rec->log_tid_;
        switch (rec->log_type_) {
            case LogType::commit:
            case LogType::ABORT:
                att_.erase(tid);
                break;
            case LogType::begin:
            case LogType::INSERT:
            case LogType::DELETE:
            case LogType::UPDATE:
                att_[tid] = rec->lsn_;
                break;
            default:
                break;
        }
    }
}

// PLACEHOLDER_REDO

/**
 * @description: 重做所有未落盘的操作（物理redo，按lsn顺序）
 * 通过比较页面的page_lsn与日志lsn判断该操作是否已经落盘，保证幂等。
 */
void RecoveryManager::redo() {
    for (auto* rec : log_records_) {
        switch (rec->log_type_) {
            case LogType::INSERT: {
                auto* log = static_cast<InsertLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                // 合并为单次 fetch：检查 page_lsn + 插入 + 设置 lsn
                RmPageHandle ph = fh->fetch_page_handle(log->rid_.page_no);
                if (ph.page->get_page_lsn() >= log->lsn_) {
                    sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), false);
                    break;
                }
                if (!Bitmap::is_set(ph.bitmap, log->rid_.slot_no)) {
                    Bitmap::set(ph.bitmap, log->rid_.slot_no);
                    ph.page_hdr->num_records++;
                }
                memcpy(ph.get_slot(log->rid_.slot_no), log->insert_value_.data, fh->get_file_hdr().record_size);
                ph.page->set_page_lsn(log->lsn_);
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), true);
                break;
            }
            case LogType::DELETE: {
                auto* log = static_cast<DeleteLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                // 合并为单次 fetch：检查 page_lsn + 删除 + 设置 lsn
                RmPageHandle ph = fh->fetch_page_handle(log->rid_.page_no);
                if (ph.page->get_page_lsn() >= log->lsn_) {
                    sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), false);
                    break;
                }
                Bitmap::reset(ph.bitmap, log->rid_.slot_no);
                ph.page_hdr->num_records--;
                ph.page->set_page_lsn(log->lsn_);
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), true);
                break;
            }
            case LogType::UPDATE: {
                auto* log = static_cast<UpdateLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                // 合并为单次 fetch：检查 page_lsn + 更新 + 设置 lsn
                RmPageHandle ph = fh->fetch_page_handle(log->rid_.page_no);
                if (ph.page->get_page_lsn() >= log->lsn_) {
                    sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), false);
                    break;
                }
                memcpy(ph.get_slot(log->rid_.slot_no), log->new_value_.data, fh->get_file_hdr().record_size);
                ph.page->set_page_lsn(log->lsn_);
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), true);
                break;
            }
            default:
                break;
        }
    }
}

// PLACEHOLDER_UNDO

/**
 * @description: 回滚未完成的事务（物理undo，按lsn逆序）
 * 对ATT中所有事务的操作逆序撤销：insert->delete, delete->insert, update->restore old。
 * 同时写补偿日志(CLR)维护日志有效性，并为每个被回滚事务追加abort日志。
 */
void RecoveryManager::undo() {
    if (att_.empty()) {
        rebuild_all_indexes();
        return;
    }

    // 收集所有活跃事务的写操作日志，按lsn逆序撤销
    std::vector<LogRecord*> to_undo;
    for (auto* rec : log_records_) {
        if (att_.find(rec->log_tid_) == att_.end()) {
            continue;
        }
        if (rec->log_type_ == LogType::INSERT || rec->log_type_ == LogType::DELETE ||
            rec->log_type_ == LogType::UPDATE) {
            to_undo.push_back(rec);
        }
    }
    std::sort(to_undo.begin(), to_undo.end(),
              [](LogRecord* a, LogRecord* b) { return a->lsn_ > b->lsn_; });

    for (auto* rec : to_undo) {
        switch (rec->log_type_) {
            case LogType::INSERT: {
                auto* log = static_cast<InsertLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                if (fh->is_record(log->rid_)) {
                    fh->delete_record(log->rid_, nullptr);
                }
                if (log_manager_ != nullptr) {
                    DeleteLogRecord clr(log->log_tid_, log->insert_value_, log->rid_, tab_name);
                    lsn_t lsn = log_manager_->add_log_to_buffer(&clr);
                    fh->set_page_lsn(log->rid_.page_no, lsn);
                }
                break;
            }
            case LogType::DELETE: {
                auto* log = static_cast<DeleteLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                if (!fh->is_record(log->rid_)) {
                    fh->insert_record(log->rid_, log->delete_value_.data);
                }
                if (log_manager_ != nullptr) {
                    InsertLogRecord clr(log->log_tid_, log->delete_value_, log->rid_, tab_name);
                    lsn_t lsn = log_manager_->add_log_to_buffer(&clr);
                    fh->set_page_lsn(log->rid_.page_no, lsn);
                }
                break;
            }
            case LogType::UPDATE: {
                auto* log = static_cast<UpdateLogRecord*>(rec);
                std::string tab_name(log->table_name_, log->table_name_size_);
                RmFileHandle* fh = get_file_handle(tab_name);
                if (fh == nullptr) break;
                if (fh->is_record(log->rid_)) {
                    fh->update_record(log->rid_, log->old_value_.data, nullptr);
                }
                if (log_manager_ != nullptr) {
                    UpdateLogRecord clr(log->log_tid_, log->new_value_, log->old_value_, log->rid_, tab_name);
                    lsn_t lsn = log_manager_->add_log_to_buffer(&clr);
                    fh->set_page_lsn(log->rid_.page_no, lsn);
                }
                break;
            }
            default:
                break;
        }
    }

    // 为每个被回滚的事务写abort日志
    if (log_manager_ != nullptr) {
        for (auto& entry : att_) {
            AbortLogRecord abort_rec(entry.first);
            log_manager_->add_log_to_buffer(&abort_rec);
        }
        log_manager_->flush_log_to_disk();
    }

    // 恢复结束后重建索引，保证索引与堆数据一致
    rebuild_all_indexes();
}

// PLACEHOLDER_REBUILD

/**
 * @description: 恢复结束后，根据恢复后的堆数据重建所有表的索引，保证索引一致性
 */
void RecoveryManager::rebuild_all_indexes() {
    for (const auto& entry : sm_manager_->db_.get_tables()) {
        const std::string& tab_name = entry.first;
        if (!entry.second.indexes.empty()) {
            sm_manager_->rebuild_indexes(tab_name, nullptr);
        }
    }
}
