/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::are_compatible(LockMode held, LockMode requested) const {
    switch (held) {
        case LockMode::SHARED:
            return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED;
        case LockMode::EXLUCSIVE:
            return false;
        case LockMode::INTENTION_SHARED:
            return requested != LockMode::EXLUCSIVE;
        case LockMode::INTENTION_EXCLUSIVE:
            return requested == LockMode::INTENTION_SHARED || requested == LockMode::INTENTION_EXCLUSIVE;
        case LockMode::S_IX:
            return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

bool LockManager::covers(LockMode held, LockMode requested) const {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held == LockMode::S_IX) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

LockManager::LockMode LockManager::combine_modes(LockMode held, LockMode requested) const {
    if (covers(held, requested)) {
        return held;
    }
    if (covers(requested, held)) {
        return requested;
    }
    if ((held == LockMode::SHARED && requested == LockMode::INTENTION_EXCLUSIVE) ||
        (held == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::SHARED)) {
        return LockMode::S_IX;
    }
    if ((held == LockMode::INTENTION_SHARED && requested == LockMode::SHARED) ||
        (held == LockMode::SHARED && requested == LockMode::INTENTION_SHARED)) {
        return LockMode::SHARED;
    }
    if ((held == LockMode::INTENTION_SHARED && requested == LockMode::INTENTION_EXCLUSIVE) ||
        (held == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::INTENTION_SHARED)) {
        return LockMode::INTENTION_EXCLUSIVE;
    }
    return requested;
}

LockManager::GroupLockMode LockManager::group_mode_of(LockMode lock_mode) const {
    switch (lock_mode) {
        case LockMode::SHARED:
            return GroupLockMode::S;
        case LockMode::EXLUCSIVE:
            return GroupLockMode::X;
        case LockMode::INTENTION_SHARED:
            return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE:
            return GroupLockMode::IX;
        case LockMode::S_IX:
            return GroupLockMode::SIX;
    }
    return GroupLockMode::NON_LOCK;
}

void LockManager::recompute_group_mode(LockRequestQueue *queue) {
    GroupLockMode mode = GroupLockMode::NON_LOCK;
    for (const auto &request : queue->request_queue_) {
        if (!request.granted_) {
            continue;
        }
        GroupLockMode request_mode = group_mode_of(request.lock_mode_);
        if (request_mode == GroupLockMode::X) {
            mode = GroupLockMode::X;
            break;
        }
        if (request_mode == GroupLockMode::SIX) {
            mode = GroupLockMode::SIX;
            continue;
        }
        if (request_mode == GroupLockMode::S && mode != GroupLockMode::SIX) {
            mode = GroupLockMode::S;
            continue;
        }
        if (request_mode == GroupLockMode::IX && mode != GroupLockMode::SIX && mode != GroupLockMode::S) {
            mode = GroupLockMode::IX;
            continue;
        }
        if (request_mode == GroupLockMode::IS && mode == GroupLockMode::NON_LOCK) {
            mode = GroupLockMode::IS;
        }
    }
    queue->group_lock_mode_ = mode;
}

bool LockManager::lock_impl(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lock(latch_);
    auto &queue = lock_table_[lock_data_id];
    auto request_it = queue.request_queue_.end();
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            request_it = it;
            break;
        }
    }

    if (request_it != queue.request_queue_.end()) {
        LockMode new_mode = combine_modes(request_it->lock_mode_, lock_mode);
        if (new_mode == request_it->lock_mode_) {
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
        for (const auto &request : queue.request_queue_) {
            if (request.txn_id_ == txn->get_transaction_id()) {
                continue;
            }
            if (!are_compatible(request.lock_mode_, new_mode) || !are_compatible(new_mode, request.lock_mode_)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
            }
        }
        request_it->lock_mode_ = new_mode;
        request_it->granted_ = true;
        recompute_group_mode(&queue);
        txn->get_lock_set()->insert(lock_data_id);
        return true;
    }

    for (const auto &request : queue.request_queue_) {
        if (!are_compatible(request.lock_mode_, lock_mode) || !are_compatible(lock_mode, request.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    recompute_group_mode(&queue);
    txn->get_lock_set()->insert(lock_data_id);
    txn->set_state(TransactionState::GROWING);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IS_on_table(txn, tab_fd);
    return lock_impl(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IX_on_table(txn, tab_fd);
    return lock_impl(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    std::unique_lock<std::mutex> lock(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) {
        return false;
    }

    auto &queue = table_it->second;
    auto request_it = queue.request_queue_.begin();
    for (; request_it != queue.request_queue_.end(); ++request_it) {
        if (request_it->txn_id_ == txn->get_transaction_id()) {
            break;
        }
    }
    if (request_it == queue.request_queue_.end()) {
        return false;
    }

    queue.request_queue_.erase(request_it);
    recompute_group_mode(&queue);
    if (queue.request_queue_.empty()) {
        lock_table_.erase(table_it);
    }
    txn->get_lock_set()->erase(lock_data_id);
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }
    return true;
}
