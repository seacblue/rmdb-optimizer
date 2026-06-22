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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;                              // 排序列的元信息（单键排序）
    bool is_desc_;
    std::vector<std::unique_ptr<RmRecord>> buffered_;
    size_t idx_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        sort_col_ = prev_->get_col_offset(sel_cols);
        is_desc_ = is_desc;
    }

    void beginTuple() override {
        buffered_.clear();
        idx_ = 0;
        // 从子结点读取所有元组，放入 buffer
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            buffered_.push_back(std::move(rec));
            prev_->nextTuple();
        }
        // 排序
        std::sort(buffered_.begin(), buffered_.end(),
                  [this](const std::unique_ptr<RmRecord> &a,
                         const std::unique_ptr<RmRecord> &b) {
                      const char *a_val = a->data + sort_col_.offset;
                      const char *b_val = b->data + sort_col_.offset;
                      int cmp = compare_value(a_val, b_val, sort_col_.type, sort_col_.len);
                      return is_desc_ ? (cmp > 0) : (cmp < 0);
                  });
    }

    void nextTuple() override {
        if (idx_ < buffered_.size()) idx_++;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (idx_ >= buffered_.size()) return nullptr;
        return std::make_unique<RmRecord>(buffered_[idx_]->size, buffered_[idx_]->data);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return idx_ >= buffered_.size(); }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

   private:
    static int compare_value(const char *a, const char *b, ColType type, int len) {
        if (type == TYPE_INT) {
            int ia = *(const int *)a, ib = *(const int *)b;
            return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
        }
        if (type == TYPE_FLOAT) {
            float fa = *(const float *)a, fb = *(const float *)b;
            if (fabs(fa - fb) < 1e-9) return 0;
            return (fa < fb) ? -1 : 1;
        }
        return strncmp(a, b, len);
    }
};