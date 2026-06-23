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
#include <unordered_map>
#include <unordered_set>
#include "analyze/analyze.h"
#include "common/type_cast.h"
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
        if (rids_.empty()) return nullptr;
        std::unordered_map<std::string, size_t> col_offset;
        for (size_t i = 0; i < tab_.cols.size(); ++i) {
            col_offset[tab_.cols[i].name] = i;
            col_offset[tab_.cols[i].tab_name + "." + tab_.cols[i].name] = i;
        }

        struct PreparedUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_rec;
            std::unique_ptr<RmRecord> new_rec;
            std::vector<std::string> old_keys;
            std::vector<std::string> new_keys;
        };

        std::vector<PreparedUpdate> prepared;
        prepared.reserve(rids_.size());
        std::vector<std::unordered_set<std::string>> seen_new_keys(tab_.indexes.size());

        for (auto &rid : rids_) {
            PreparedUpdate item;
            item.rid = rid;
            item.old_rec = fh_->get_record(rid, context_);
            item.new_rec = std::make_unique<RmRecord>(*item.old_rec);

            for (auto &clause : set_clauses_) {
                auto it = get_col(tab_.cols, clause.lhs);
                Value rhs_value = clause.rhs_expr != nullptr
                    ? eval_set_expr(clause.rhs_expr, item.new_rec->data, tab_.cols, col_offset)
                    : clause.rhs;
                rhs_value = TypeCaster::cast_value(rhs_value, it->type, it->len);
                rhs_value.init_raw(it->len);
                memcpy(item.new_rec->data + it->offset, rhs_value.raw->data, it->len);
            }

            for (size_t j = 0; j < tab_.indexes.size(); ++j) {
                auto &index = tab_.indexes[j];
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                std::string old_key;
                std::string new_key;
                old_key.reserve(index.col_tot_len);
                new_key.reserve(index.col_tot_len);
                for (size_t k = 0; k < (size_t)index.col_num; ++k) {
                    old_key.append(item.old_rec->data + index.cols[k].offset, index.cols[k].len);
                    new_key.append(item.new_rec->data + index.cols[k].offset, index.cols[k].len);
                }
                item.old_keys.push_back(old_key);
                item.new_keys.push_back(new_key);

                if (old_key != new_key) {
                    if (!seen_new_keys[j].insert(new_key).second) {
                        throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                    }
                    std::vector<Rid> result;
                    if (ih->get_value(new_key.data(), &result, context_->txn_)) {
                        throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
                    }
                }
            }

            prepared.push_back(std::move(item));
        }

        for (auto &item : prepared) {
            for (size_t j = 0; j < tab_.indexes.size(); ++j) {
                auto &index = tab_.indexes[j];
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                if (item.old_keys[j] != item.new_keys[j]) {
                    ih->delete_entry(item.old_keys[j].data(), context_->txn_);
                }
            }

            for (size_t j = 0; j < tab_.indexes.size(); ++j) {
                auto &index = tab_.indexes[j];
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                if (item.old_keys[j] != item.new_keys[j]) {
                    ih->insert_entry(item.new_keys[j].data(), item.rid, context_->txn_);
                }
            }

            fh_->update_record(item.rid, item.new_rec->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
