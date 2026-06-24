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

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "common/type_cast.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t kJoinBufferBytes = 512 * 1024 * 1024;

    struct CompiledCond {
        bool lhs_from_left = true;
        int lhs_offset = 0;
        ColType lhs_type = TYPE_INT;
        int lhs_len = 0;
        CompOp op = OP_EQ;
        bool rhs_is_val = true;
        bool rhs_from_left = false;
        int rhs_offset = 0;
        ColType rhs_type = TYPE_INT;
        int rhs_len = 0;
        Value rhs_val;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    size_t left_tuple_len_ = 0;
    size_t right_tuple_len_ = 0;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> left_cols_;
    std::vector<ColMeta> right_cols_;
    std::vector<Condition> fed_conds_;
    std::vector<CompiledCond> compiled_conds_;

    std::vector<char> left_block_data_;
    size_t left_block_count_ = 0;
    size_t left_block_idx_ = 0;
    std::unique_ptr<RmRecord> current_right_;
    std::unique_ptr<RmRecord> current_joined_;
    bool right_ready_ = false;
    bool isend_ = true;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds)
        : left_(std::move(left)), right_(std::move(right)), fed_conds_(std::move(conds)) {
        left_cols_ = left_->cols();
        right_cols_ = right_->cols();
        left_tuple_len_ = left_->tupleLen();
        right_tuple_len_ = right_->tupleLen();
        len_ = left_tuple_len_ + right_tuple_len_;
        cols_ = left_cols_;
        auto shifted_right_cols = right_cols_;
        for (auto &col : shifted_right_cols) {
            col.offset += static_cast<int>(left_tuple_len_);
        }
        cols_.insert(cols_.end(), shifted_right_cols.begin(), shifted_right_cols.end());
        compile_conds();
    }

    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds, Context *context)
        : NestedLoopJoinExecutor(std::move(left), std::move(right), std::move(conds)) {
        context_ = context;
    }

    void beginTuple() override {
        left_->beginTuple();
        left_block_data_.clear();
        left_block_count_ = 0;
        left_block_idx_ = 0;
        current_right_.reset();
        current_joined_.reset();
        right_ready_ = false;

        if (!load_next_left_block()) {
            isend_ = true;
            return;
        }
        right_->beginTuple();
        if (!right_->is_end()) {
            current_right_ = right_->Next();
            right_ready_ = true;
        }
        isend_ = false;
        advance_to_match();
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        ++left_block_idx_;
        advance_to_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_ || current_joined_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_joined_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    bool load_next_left_block() {
        left_block_data_.clear();
        left_block_count_ = 0;
        left_block_idx_ = 0;

        const size_t tuple_len = std::max<size_t>(1, left_tuple_len_);
        const size_t capacity = std::max<size_t>(1, kJoinBufferBytes / tuple_len);
        left_block_data_.reserve(capacity * tuple_len);

        while (!left_->is_end() && left_block_count_ < capacity) {
            auto rec = left_->Next();
            if (rec != nullptr) {
                size_t old_size = left_block_data_.size();
                left_block_data_.resize(old_size + tuple_len);
                memcpy(left_block_data_.data() + old_size, rec->data, tuple_len);
                ++left_block_count_;
            }
            left_->nextTuple();
        }
        return left_block_count_ > 0;
    }

    const ColMeta *find_col(const std::vector<ColMeta> &source, const TabCol &target) const {
        auto it = std::find_if(source.begin(), source.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (it == source.end()) {
            return nullptr;
        }
        return &(*it);
    }

    const char *left_tuple_at(size_t idx) const {
        return left_block_data_.data() + idx * left_tuple_len_;
    }

    bool is_satisfied(const char *left_rec, const char *right_rec) const {
        for (const auto &cond : compiled_conds_) {
            const char *lhs_ptr = cond.lhs_from_left ? (left_rec + cond.lhs_offset) : (right_rec + cond.lhs_offset);
            int cmp = 0;
            if (cond.rhs_is_val) {
                cmp = TypeCaster::compare_raw_with_value(lhs_ptr, cond.lhs_type, cond.lhs_len, cond.rhs_val);
            } else {
                const char *rhs_ptr = cond.rhs_from_left ? (left_rec + cond.rhs_offset) : (right_rec + cond.rhs_offset);
                cmp = TypeCaster::compare_raw(lhs_ptr, cond.lhs_type, cond.lhs_len, rhs_ptr, cond.rhs_type, cond.rhs_len);
            }
            if (!compare_by_op(cmp, cond.op)) {
                return false;
            }
        }
        return true;
    }

    void build_joined_tuple(const char *left_rec, const char *right_rec) {
        current_joined_ = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(current_joined_->data, left_rec, left_tuple_len_);
        memcpy(current_joined_->data + left_tuple_len_, right_rec, right_tuple_len_);
    }

    void advance_to_match() {
        current_joined_.reset();
        while (true) {
            if (left_block_count_ == 0) {
                isend_ = true;
                return;
            }

            if (!right_ready_) {
                if (right_->is_end()) {
                    if (!load_next_left_block()) {
                        isend_ = true;
                        return;
                    }
                    right_->beginTuple();
                    if (right_->is_end()) {
                        isend_ = true;
                        return;
                    }
                }
                current_right_ = right_->Next();
                right_ready_ = true;
                left_block_idx_ = 0;
            }

            while (left_block_idx_ < left_block_count_) {
                const char *left_rec = left_tuple_at(left_block_idx_);
                if (is_satisfied(left_rec, current_right_->data)) {
                    build_joined_tuple(left_rec, current_right_->data);
                    isend_ = false;
                    return;
                }
                ++left_block_idx_;
            }

            left_block_idx_ = 0;
            right_->nextTuple();
            right_ready_ = false;
        }
    }

    void compile_conds() {
        compiled_conds_.clear();
        compiled_conds_.reserve(fed_conds_.size());
        for (const auto &cond : fed_conds_) {
            CompiledCond cc;
            cc.op = cond.op;

            if (const ColMeta *lhs_col = find_col(left_cols_, cond.lhs_col)) {
                cc.lhs_from_left = true;
                cc.lhs_offset = lhs_col->offset;
                cc.lhs_type = lhs_col->type;
                cc.lhs_len = lhs_col->len;
            } else if (const ColMeta *lhs_col = find_col(right_cols_, cond.lhs_col)) {
                cc.lhs_from_left = false;
                cc.lhs_offset = lhs_col->offset;
                cc.lhs_type = lhs_col->type;
                cc.lhs_len = lhs_col->len;
            } else {
                throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
            }

            cc.rhs_is_val = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                cc.rhs_val = cond.rhs_val;
            } else if (const ColMeta *rhs_col = find_col(left_cols_, cond.rhs_col)) {
                cc.rhs_from_left = true;
                cc.rhs_offset = rhs_col->offset;
                cc.rhs_type = rhs_col->type;
                cc.rhs_len = rhs_col->len;
            } else if (const ColMeta *rhs_col = find_col(right_cols_, cond.rhs_col)) {
                cc.rhs_from_left = false;
                cc.rhs_offset = rhs_col->offset;
                cc.rhs_type = rhs_col->type;
                cc.rhs_len = rhs_col->len;
            } else {
                throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
            }
            compiled_conds_.push_back(cc);
        }
    }
};
