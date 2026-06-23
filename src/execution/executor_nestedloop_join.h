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
#include "common/type_cast.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) { isend = true; return; }
        right_->beginTuple();
        if (right_->is_end()) { isend = true; return; }
        isend = false;
        advance_to_match();
    }

    void nextTuple() override {
        if (isend) return;
        right_->nextTuple();
        if (right_->is_end()) {
            left_->nextTuple();
            if (left_->is_end()) { isend = true; return; }
            right_->beginTuple();
            if (right_->is_end()) { isend = true; return; }
        }
        advance_to_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    /** @brief 推进到下一对满足 join 条件的 (left,right) 组合 */
    void advance_to_match() {
        while (!left_->is_end() && !right_->is_end()) {
            if (fed_conds_.empty()) return;   // 无条件 → 当前对有效

            // 拼接当前左右记录
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();
            auto rec = std::make_unique<RmRecord>(len_);
            memcpy(rec->data, left_rec->data, left_->tupleLen());
            memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());

            if (eval_join_conds(rec->data, fed_conds_, cols_)) return;

            // 不匹配 → 推进右表
            right_->nextTuple();
            if (right_->is_end()) {
                left_->nextTuple();
                if (left_->is_end()) { isend = true; return; }
                right_->beginTuple();
            }
        }
        isend = true;
    }

    /** @brief 求值一组连接条件（AND 语义） */
    static bool eval_join_conds(const char *rec_data, const std::vector<Condition> &conds,
                                const std::vector<ColMeta> &cols) {
        if (conds.empty()) return true;
        for (auto &cond : conds) {
            auto lhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                return c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name;
            });
            if (lhs_it == cols.end()) return false;

            ColType lhs_type = lhs_it->type;
            int len = lhs_it->len;
            const char *lhs_val = rec_data + lhs_it->offset;

            int cmp = 0;
            if (cond.is_rhs_val) {
                cmp = TypeCaster::compare_raw_with_value(lhs_val, lhs_type, len, cond.rhs_val);
            } else {
                auto rhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                    return c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name;
                });
                if (rhs_it == cols.end()) return false;
                cmp = TypeCaster::compare_raw(lhs_val, lhs_type, len,
                                              rec_data + rhs_it->offset, rhs_it->type, rhs_it->len);
            }

            bool ok = false;
            switch (cond.op) {
                case OP_EQ: ok = (cmp == 0); break;
                case OP_NE: ok = (cmp != 0); break;
                case OP_LT: ok = (cmp <  0); break;
                case OP_GT: ok = (cmp >  0); break;
                case OP_LE: ok = (cmp <= 0); break;
                case OP_GE: ok = (cmp >= 0); break;
            }
            if (!ok) return false;
        }
        return true;
    }
};
