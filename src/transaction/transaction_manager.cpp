/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "recovery/log_manager.h"
#include "common/config.h"

namespace {

void clear_write_set(const std::shared_ptr<std::deque<WriteRecord *>> &write_set) {
    for (auto *write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
}

void release_all_locks(Transaction *txn, LockManager *lock_manager) {
    if (txn == nullptr || lock_manager == nullptr) {
        return;
    }
    std::vector<LockDataId> lock_ids;
    lock_ids.reserve(txn->get_lock_set()->size());
    for (const auto &lock_id : *txn->get_lock_set()) {
        lock_ids.push_back(lock_id);
    }
    for (const auto &lock_id : lock_ids) {
        lock_manager->unlock(txn, lock_id);
    }
}

}  // namespace

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        // 创建新事务
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
        txn->set_start_ts(next_timestamp_++);
        txn->set_state(TransactionState::GROWING);
    }
    // 写begin日志
    if (enable_logging && log_manager != nullptr) {
        BeginLogRecord log_rec(txn->get_transaction_id());
        log_rec.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_rec);
        txn->set_prev_lsn(lsn);
    }
    // 把开始事务加入到全局事务表中
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    lock.unlock();
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    // 写commit日志并强制刷盘（WAL: 事务提交前其所有日志必须落盘）
    if (enable_logging && log_manager != nullptr) {
        CommitLogRecord log_rec(txn->get_transaction_id());
        log_rec.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_rec);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    clear_write_set(txn->get_write_set());
    release_all_locks(txn, lock_manager_);
    txn->set_state(TransactionState::COMMITTED);
    // 从事务表中移除
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }

    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *write_record = *it;
        const std::string &tab_name = write_record->GetTableName();
        auto fh_it = sm_manager_->fhs_.find(tab_name);
        if (fh_it == sm_manager_->fhs_.end()) {
            continue;
        }
        RmFileHandle *fh = fh_it->second.get();
        Rid rid = write_record->GetRid();
        bool do_log = enable_logging && log_manager != nullptr;
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // undo insert: 删除该记录，补偿日志为delete
                auto rec = fh->get_record(rid, nullptr);
                sm_manager_->delete_index_entries(tab_name, *rec, rid, nullptr);
                fh->delete_record(rid, nullptr);
                if (do_log) {
                    DeleteLogRecord clr(txn->get_transaction_id(), *rec, rid, tab_name);
                    clr.prev_lsn_ = txn->get_prev_lsn();
                    lsn_t lsn = log_manager->add_log_to_buffer(&clr);
                    txn->set_prev_lsn(lsn);
                    fh->set_page_lsn(rid.page_no, lsn);
                }
                break;
            }
            case WType::DELETE_TUPLE: {
                // undo delete: 重新插入该记录，补偿日志为insert
                fh->insert_record(rid, write_record->GetRecord().data);
                sm_manager_->insert_index_entries(tab_name, write_record->GetRecord(), rid, nullptr);
                if (do_log) {
                    InsertLogRecord clr(txn->get_transaction_id(), write_record->GetRecord(), rid, tab_name);
                    clr.prev_lsn_ = txn->get_prev_lsn();
                    lsn_t lsn = log_manager->add_log_to_buffer(&clr);
                    txn->set_prev_lsn(lsn);
                    fh->set_page_lsn(rid.page_no, lsn);
                }
                break;
            }
            case WType::UPDATE_TUPLE: {
                // undo update: 恢复旧值，补偿日志为update(new->old)
                auto cur = fh->get_record(rid, nullptr);
                sm_manager_->update_index_entries(tab_name, *cur, write_record->GetRecord(), rid, nullptr);
                fh->update_record(rid, write_record->GetRecord().data, nullptr);
                if (do_log) {
                    UpdateLogRecord clr(txn->get_transaction_id(), *cur, write_record->GetRecord(), rid, tab_name);
                    clr.prev_lsn_ = txn->get_prev_lsn();
                    lsn_t lsn = log_manager->add_log_to_buffer(&clr);
                    txn->set_prev_lsn(lsn);
                    fh->set_page_lsn(rid.page_no, lsn);
                }
                break;
            }
        }
    }

    // 写abort日志并强制刷盘
    if (enable_logging && log_manager != nullptr) {
        AbortLogRecord log_rec(txn->get_transaction_id());
        log_rec.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_rec);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }

    clear_write_set(write_set);
    release_all_locks(txn, lock_manager_);
    txn->set_state(TransactionState::ABORTED);
    // 从事务表中移除
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}
