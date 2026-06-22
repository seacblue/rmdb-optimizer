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
#include <cmath>
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

            std::unique_ptr<char[]> rhs_buf;
            const char *rhs_val = nullptr;
            ColType rhs_type = lhs_type;
            if (cond.is_rhs_val) {
                rhs_type = cond.rhs_val.type;
                rhs_buf = std::make_unique<char[]>(len);
                memset(rhs_buf.get(), 0, len);
                if (cond.rhs_val.type == TYPE_INT) {
                    *(int *)rhs_buf.get() = cond.rhs_val.int_val;
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    *(float *)rhs_buf.get() = cond.rhs_val.float_val;
                } else {
                    memcpy(rhs_buf.get(), cond.rhs_val.str_val.c_str(),
                           std::min((int)cond.rhs_val.str_val.size(), len));
                }
                rhs_val = rhs_buf.get();
            } else {
                auto rhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                    return c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name;
                });
                if (rhs_it == cols.end()) return false;
                rhs_type = rhs_it->type;
                rhs_val = rec_data + rhs_it->offset;
            }

            int cmp = compare_value(lhs_val, rhs_val, lhs_type, rhs_type, len, cond.is_rhs_val ? &cond.rhs_val : nullptr);
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

    static int compare_value(const char *a, const char *b, ColType lhs_type, ColType rhs_type, int len, const Value *rhs_literal) {
        if ((lhs_type == TYPE_INT || lhs_type == TYPE_FLOAT) &&
            (rhs_type == TYPE_INT || rhs_type == TYPE_FLOAT) &&
            lhs_type != rhs_type) {
            double lhs_num = lhs_type == TYPE_FLOAT ? static_cast<double>(*reinterpret_cast<const float *>(a))
                                                    : static_cast<double>(*reinterpret_cast<const int *>(a));
            double rhs_num = 0.0;
            if (rhs_literal != nullptr) {
                rhs_num = rhs_type == TYPE_FLOAT ? static_cast<double>(rhs_literal->float_val)
                                                 : static_cast<double>(rhs_literal->int_val);
            } else {
                rhs_num = rhs_type == TYPE_FLOAT ? static_cast<double>(*reinterpret_cast<const float *>(b))
                                                 : static_cast<double>(*reinterpret_cast<const int *>(b));
            }
            if (fabs(lhs_num - rhs_num) < 1e-9) return 0;
            return lhs_num < rhs_num ? -1 : 1;
        }
        if (lhs_type == TYPE_INT) {
            int ia = *(const int *)a, ib = *(const int *)b;
            return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
        }
        if (lhs_type == TYPE_FLOAT) {
            float fa = *(const float *)a, fb = *(const float *)b;
            if (fabs(fa - fb) < 1e-9) return 0;
            return (fa < fb) ? -1 : 1;
        }
        return strncmp(a, b, len);
    }
};
