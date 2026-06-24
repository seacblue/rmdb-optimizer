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
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int limit_count_;
    std::vector<std::unique_ptr<RmRecord>> sorted_;
    size_t cursor_ = 0;
    std::vector<ColMeta> cols_;
    size_t tuple_len_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols,
                 const std::vector<bool> &is_desc, int limit_count) {
        prev_ = std::move(prev);
        is_desc_ = is_desc;
        limit_count_ = limit_count;
        cols_ = prev_->cols();
        tuple_len_ = prev_->tupleLen();
        for (const auto &sel_col : sel_cols) {
            sort_cols_.push_back(prev_->get_col_offset(sel_col));
        }
    }

    void beginTuple() override {
        sorted_.clear();
        cursor_ = 0;
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            sorted_.push_back(std::make_unique<RmRecord>(*rec));
            prev_->nextTuple();
        }

        std::stable_sort(sorted_.begin(), sorted_.end(),
                         [&](const std::unique_ptr<RmRecord> &lhs, const std::unique_ptr<RmRecord> &rhs) {
                             for (size_t i = 0; i < sort_cols_.size(); ++i) {
                                 const auto &col = sort_cols_[i];
                                 Value l = Value::from_raw(col.type, lhs->data + col.offset, col.len);
                                 Value r = Value::from_raw(col.type, rhs->data + col.offset, col.len);
                                 int cmp = compare_values(l, r);
                                 if (cmp == 0) {
                                     continue;
                                 }
                                 return is_desc_[i] ? (cmp > 0) : (cmp < 0);
                             }
                             return false;
                         });

        if (limit_count_ >= 0 && static_cast<int>(sorted_.size()) > limit_count_) {
            sorted_.resize(static_cast<size_t>(limit_count_));
        }
    }

    void nextTuple() override {
        if (cursor_ < sorted_.size()) {
            ++cursor_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*sorted_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cursor_ >= sorted_.size(); }

    size_t tupleLen() const override { return tuple_len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
