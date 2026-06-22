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
#include <climits>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    void beginTuple() override { done_ = false; }
    void nextTuple() override { done_ = true; }
    bool is_end() const override { return done_; }

    std::unique_ptr<RmRecord> Next() override {
        if (done_) return nullptr;
        done_ = true;
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            // Allow implicit conversion between INT and BIGINT
            if (col.type != val.type) {
                if (col.type == TYPE_BIGINT && val.type == TYPE_INT) {
                    // Widen int to bigint
                    val.set_bigint(val.int_val);
                } else if (col.type == TYPE_INT && val.type == TYPE_BIGINT) {
                    // Check if bigint value fits in int range
                    if (val.bigint_val > INT_MAX || val.bigint_val < INT_MIN) {
                        throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                    }
                    val.set_int(static_cast<int>(val.bigint_val));
                } else {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);
        
        // Insert into index
        for(size_t j = 0; j < tab_.indexes.size(); ++j) {
            auto& index = tab_.indexes[j];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            auto key = std::make_unique<char[]>(index.col_tot_len);
            int offset = 0;
            for(size_t k = 0; k < index.col_num; ++k) {
                memcpy(key.get() + offset, rec.data + index.cols[k].offset, index.cols[k].len);
                offset += index.cols[k].len;
            }
            ih->insert_entry(key.get(), rid_, context_->txn_);
        }
        return std::make_unique<RmRecord>(rec);
    }
    Rid &rid() override { return rid_; }

   private:
    bool done_ = false;
};