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
#include "analyze/analyze.h"
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

        for (auto &rid : rids_) {
            // 读取当前记录
            auto rec = fh_->get_record(rid, context_);

            // 删除旧的索引项
            for (size_t j = 0; j < tab_.indexes.size(); ++j) {
                auto &index = tab_.indexes[j];
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                auto old_key = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t k = 0; k < index.col_num; ++k) {
                    memcpy(old_key.get() + offset, rec->data + index.cols[k].offset, index.cols[k].len);
                    offset += index.cols[k].len;
                }
                ih->delete_entry(old_key.get(), context_->txn_);
            }

            // 应用 SET 子句
            for (auto &clause : set_clauses_) {
                auto it = get_col(tab_.cols, clause.lhs);
                Value rhs_value = clause.rhs_expr != nullptr
                    ? eval_set_expr(clause.rhs_expr, rec->data, tab_.cols, col_offset)
                    : clause.rhs;
                if (it->type == TYPE_FLOAT && rhs_value.type == TYPE_INT) {
                    rhs_value.set_float(static_cast<float>(rhs_value.int_val));
                } else if (it->type == TYPE_INT && rhs_value.type == TYPE_FLOAT) {
                    rhs_value.set_int(static_cast<int>(rhs_value.float_val));
                }
                rhs_value.init_raw(it->len);
                memcpy(rec->data + it->offset, rhs_value.raw->data, it->len);
            }

            // 插入新的索引项
            for (size_t j = 0; j < tab_.indexes.size(); ++j) {
                auto &index = tab_.indexes[j];
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                auto new_key = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t k = 0; k < index.col_num; ++k) {
                    memcpy(new_key.get() + offset, rec->data + index.cols[k].offset, index.cols[k].len);
                    offset += index.cols[k].len;
                }
                ih->insert_entry(new_key.get(), rid, context_->txn_);
            }

            // 写回
            fh_->update_record(rid, rec->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
