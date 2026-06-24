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
#include "common/type_cast.h"

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

    bool eval_cond(const char *rec_data, const Condition &cond) {
        auto lhs_col = get_col(cols_, cond.lhs_col);

        if (cond.is_rhs_val) {
            int cmp = TypeCaster::compare_raw_with_value(
                rec_data + lhs_col->offset, lhs_col->type, lhs_col->len, cond.rhs_val);
            return compare_by_op(cmp, cond.op);
        }

        auto rhs_col = get_col(cols_, cond.rhs_col);
        int cmp = TypeCaster::compare_raw(
            rec_data + lhs_col->offset, lhs_col->type, lhs_col->len,
            rec_data + rhs_col->offset, rhs_col->type, rhs_col->len);
        return compare_by_op(cmp, cond.op);
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
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        scan_ = std::make_unique<RmScan>(fh_);
        // 跳过不满足条件的记录 — 直接访问页面数据，避免get_record的堆分配开销
        while (!scan_->is_end()) {
            Rid cur_rid = scan_->rid();
            RmPageHandle page_handle = fh_->fetch_page_handle(cur_rid.page_no);
            bool matched = is_satisfied(page_handle.get_slot(cur_rid.slot_no));
            sm_manager_->get_bpm()->unpin_page(page_handle.page->get_page_id(), false);
            if (matched) {
                rid_ = cur_rid;
                break;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            Rid cur_rid = scan_->rid();
            RmPageHandle page_handle = fh_->fetch_page_handle(cur_rid.page_no);
            bool matched = is_satisfied(page_handle.get_slot(cur_rid.slot_no));
            sm_manager_->get_bpm()->unpin_page(page_handle.page->get_page_id(), false);
            if (matched) {
                rid_ = cur_rid;
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
