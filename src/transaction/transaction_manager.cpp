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

namespace {

void clear_write_set(const std::shared_ptr<std::deque<WriteRecord *>> &write_set) {
    for (auto *write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
}

void release_all_locks(Transaction *txn, LockManager *lock_manager) {
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
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_start_ts(next_timestamp_++);
    txn->set_state(TransactionState::GROWING);

    if (log_manager != nullptr) {
        BeginLogRecord log(txn->get_transaction_id());
        log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&log));
    }

    std::lock_guard<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
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

    if (log_manager != nullptr) {
        CommitLogRecord log(txn->get_transaction_id());
        log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&log));
        log_manager->flush_log_to_disk();
    }

    clear_write_set(txn->get_write_set());
    release_all_locks(txn, lock_manager_);
    txn->set_state(TransactionState::COMMITTED);

    std::lock_guard<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
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
        auto fh_it = sm_manager_->fhs_.find(write_record->GetTableName());
        if (fh_it == sm_manager_->fhs_.end()) {
            continue;
        }
        RmFileHandle *fh = fh_it->second.get();
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE:
                fh->delete_record(write_record->GetRid(), nullptr);
                break;
            case WType::DELETE_TUPLE:
                fh->insert_record(write_record->GetRid(), write_record->GetRecord().data);
                break;
            case WType::UPDATE_TUPLE:
                fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr);
                break;
        }
    }

    if (log_manager != nullptr) {
        AbortLogRecord log(txn->get_transaction_id());
        log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&log));
        log_manager->flush_log_to_disk();
    }

    clear_write_set(write_set);
    release_all_locks(txn, lock_manager_);
    txn->set_state(TransactionState::ABORTED);

    std::lock_guard<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}
