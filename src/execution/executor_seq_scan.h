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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    // 比较两个值，返回比较结果：true 表示满足条件
    bool eval_cond(const char *rec_data, const Condition &cond) {
        // 获取 lhs 列的元数据
        auto lhs_col = get_col(cols_, cond.lhs_col);
        char *lhs_buf = const_cast<char *>(rec_data) + lhs_col->offset;

        if (cond.is_rhs_val) {
            // rhs 是常量值
            char *rhs_buf = cond.rhs_val.raw->data;
            return compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op, lhs_col->len);
        } else {
            // rhs 是列
            auto rhs_col = get_col(cols_, cond.rhs_col);
            char *rhs_buf = const_cast<char *>(rec_data) + rhs_col->offset;
            return compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op, lhs_col->len);
        }
    }

    // 比较两个原始值
    bool compare_value(const char *lhs, const char *rhs, ColType type, CompOp op, int len = 0) {
        int cmp_result = 0;
        if (type == TYPE_INT) {
            int l = *(int *)lhs;
            int r = *(int *)rhs;
            if (l < r) cmp_result = -1;
            else if (l > r) cmp_result = 1;
            else cmp_result = 0;
        } else if (type == TYPE_FLOAT) {
            float l = *(float *)lhs;
            float r = *(float *)rhs;
            if (l < r) cmp_result = -1;
            else if (l > r) cmp_result = 1;
            else cmp_result = 0;
        } else if (type == TYPE_STRING) {
            cmp_result = strncmp(lhs, rhs, len);
        }
        switch (op) {
            case OP_EQ: return cmp_result == 0;
            case OP_NE: return cmp_result != 0;
            case OP_LT: return cmp_result < 0;
            case OP_GT: return cmp_result > 0;
            case OP_LE: return cmp_result <= 0;
            case OP_GE: return cmp_result >= 0;
            default: return false;
        }
    }

    // 检查记录是否满足所有条件
    bool is_satisfied(const char *rec_data) {
        for (auto &cond : fed_conds_) {
            if (!eval_cond(rec_data, cond)) {
                return false;
            }
        }
        return true;
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        // 跳过不满足条件的记录
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (is_satisfied(rec->data)) {
                rid_ = scan_->rid();
                break;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (is_satisfied(rec->data)) {
                rid_ = scan_->rid();
                break;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};