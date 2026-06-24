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
#include <unordered_set>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "recovery/log_manager.h"
#include "common/config.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        }
        struct PreparedUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_rec;
            std::unique_ptr<RmRecord> new_rec;
        };

        std::vector<PreparedUpdate> prepared;
        prepared.reserve(rids_.size());
        std::unordered_set<std::string> touched_rids;

        for (auto &rid : rids_) {
            touched_rids.insert(std::to_string(rid.page_no) + "#" + std::to_string(rid.slot_no));
            auto rec = fh_->get_record(rid, context_);
            auto new_rec = std::make_unique<RmRecord>(*rec);

            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(new_rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            prepared.push_back({rid, std::move(rec), std::move(new_rec)});
        }

        for (auto &index : tab_.indexes) {
            std::unordered_set<std::string> new_keys;
            new_keys.reserve(prepared.size());

            for (auto &item : prepared) {
                char key_buf[64];
                assert(static_cast<size_t>(index.col_tot_len) <= sizeof(key_buf));
                int offset = 0;
                for (size_t k = 0; k < static_cast<size_t>(index.col_num); ++k) {
                    memcpy(key_buf + offset, item.new_rec->data + index.cols[k].offset, index.cols[k].len);
                    offset += index.cols[k].len;
                }
                std::string key_str(key_buf, index.col_tot_len);
                if (!new_keys.emplace(key_str).second) {
                    throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                }
            }

            // 检查表中现有记录是否与新的唯一键冲突
            // 使用页面直接访问避免 get_record 堆分配
            auto hdr = fh_->get_file_hdr();
            for (int page_no = RM_FIRST_RECORD_PAGE; page_no < hdr.num_pages; ++page_no) {
                RmPageHandle ph = fh_->fetch_page_handle(page_no);
                int nslots = hdr.num_records_per_page;
                bool page_dirty = false;
                for (int slot = 0; slot < nslots; ++slot) {
                    if (!Bitmap::is_set(ph.bitmap, slot)) continue;
                    Rid current = {page_no, slot};
                    std::string rid_tag = std::to_string(current.page_no) + "#" + std::to_string(current.slot_no);
                    if (touched_rids.find(rid_tag) != touched_rids.end()) continue;

                    char key_buf[64];
                    int offset = 0;
                    for (size_t k = 0; k < static_cast<size_t>(index.col_num); ++k) {
                        memcpy(key_buf + offset, ph.get_slot(slot) + index.cols[k].offset, index.cols[k].len);
                        offset += index.cols[k].len;
                    }
                    std::string key_str(key_buf, index.col_tot_len);
                    if (new_keys.find(key_str) != new_keys.end()) {
                        sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), false);
                        throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                    }
                }
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), page_dirty);
            }
        }

        for (auto &item : prepared) {
            sm_manager_->update_index_entries(tab_name_, *item.old_rec, *item.new_rec, item.rid, context_);
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, item.rid, *item.old_rec));
                // 写update日志（记录旧值和新值，分别用于undo和redo）
                if (enable_logging && context_->log_mgr_ != nullptr) {
                    UpdateLogRecord log_rec(context_->txn_->get_transaction_id(), *item.old_rec,
                                            *item.new_rec, item.rid, tab_name_);
                    log_rec.prev_lsn_ = context_->txn_->get_prev_lsn();
                    lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_rec);
                    context_->txn_->set_prev_lsn(lsn);
                    fh_->set_page_lsn(item.rid.page_no, lsn);
                }
            }
            fh_->update_record(item.rid, item.new_rec->data, context_);
        }

        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
