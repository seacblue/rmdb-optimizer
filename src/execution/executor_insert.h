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
#include <memory>
#include <vector>
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

    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        }
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto val = values_[i];
            bool compatible = (col.type == val.type) ||
                              (is_numeric_type(col.type) && val.is_numeric()) ||
                              (col.type == TYPE_DATETIME && val.type == TYPE_STRING);
            if (!compatible) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            coerce_value(val, col.type, col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        for (size_t j = 0; j < tab_.indexes.size(); ++j) {
            auto& index = tab_.indexes[j];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            auto key = std::make_unique<char[]>(index.col_tot_len);
            int offset = 0;
            for (size_t k = 0; k < static_cast<size_t>(index.col_num); ++k) {
                memcpy(key.get() + offset, rec.data + index.cols[k].offset, index.cols[k].len);
                offset += index.cols[k].len;
            }
            std::vector<Rid> result;
            if (ih->get_value(key.get(), &result, context_->txn_)) {
                throw UniqueConstraintError(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols));
            }
        }

        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);
        if (context_ != nullptr && context_->txn_ != nullptr) {
            context_->txn_->append_write_record(
                new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
        }

        if (!tab_.indexes.empty()) {
            sm_manager_->rebuild_indexes(tab_name_, context_);
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
