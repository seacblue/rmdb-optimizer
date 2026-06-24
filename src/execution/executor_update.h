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

        for (size_t j = 0; j < tab_.indexes.size(); ++j) {
            auto &index = tab_.indexes[j];
            std::unordered_set<std::string> new_keys;

            for (auto &item : prepared) {
                auto key_buf = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t k = 0; k < static_cast<size_t>(index.col_num); ++k) {
                    memcpy(key_buf.get() + offset, item.new_rec->data + index.cols[k].offset, index.cols[k].len);
                    offset += index.cols[k].len;
                }
                std::string key_str(key_buf.get(), key_buf.get() + index.col_tot_len);
                if (!new_keys.insert(key_str).second) {
                    throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                }
            }

            RmScan scan(fh_);
            while (!scan.is_end()) {
                Rid current = {.page_no = scan.rid().page_no, .slot_no = scan.rid().slot_no};
                std::string rid_tag = std::to_string(current.page_no) + "#" + std::to_string(current.slot_no);
                if (touched_rids.find(rid_tag) != touched_rids.end()) {
                    scan.next();
                    continue;
                }
                auto rec = fh_->get_record(current, context_);
                auto key_buf = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t k = 0; k < static_cast<size_t>(index.col_num); ++k) {
                    memcpy(key_buf.get() + offset, rec->data + index.cols[k].offset, index.cols[k].len);
                    offset += index.cols[k].len;
                }
                std::string key_str(key_buf.get(), key_buf.get() + index.col_tot_len);
                if (new_keys.find(key_str) != new_keys.end()) {
                    throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                }
                scan.next();
            }
        }

        for (auto &item : prepared) {
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, item.rid, *item.old_rec));
            }
            fh_->update_record(item.rid, item.new_rec->data, context_);
        }

        if (!tab_.indexes.empty()) {
            sm_manager_->rebuild_indexes(tab_name_, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
