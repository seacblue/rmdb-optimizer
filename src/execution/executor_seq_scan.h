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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同
    bool reverse_;                      // 是否反向扫描（用于嵌套连接的内表）

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context, bool reverse = false) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        reverse_ = reverse;
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_, reverse_);
        advance_to_match();
    }

    void nextTuple() override {
        if (scan_->is_end()) return;
        scan_->next();
        advance_to_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_->is_end()) return nullptr;
        rid_ = scan_->rid();
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    /** @brief 从 scan_ 当前位置开始，推进到第一条满足 conds_ 条件的记录 */
    void advance_to_match() {
        while (!scan_->is_end()) {
            if (conds_.empty()) break;   // 无条件 → 当前记录即有效
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(rec->data, conds_, cols_)) break;
            scan_->next();
        }        if (!scan_->is_end()) {
            rid_ = scan_->rid();
        }    }

    /** @brief 求值一组条件（AND 语义） */
    static bool eval_conds(const char *rec_data, const std::vector<Condition> &conds,
                           const std::vector<ColMeta> &cols) {
        if (conds.empty()) return true;
        for (auto &cond : conds) {
            // 找到左值列
            auto lhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                return c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name;
            });
            if (lhs_it == cols.end()) return false;   // 列不存在 → 不匹配
            ColType lhs_type = lhs_it->type;
            int len = lhs_it->len;
            const char *lhs_val = rec_data + lhs_it->offset;

            // 获取右值
            int cmp = 0;
            if (cond.is_rhs_val) {
                cmp = TypeCaster::compare_raw_with_value(lhs_val, lhs_type, len, cond.rhs_val);
            } else {
                // 右值也是列（列-列比较，仅在同一张表中支持）
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
            if (!ok) return false;   // AND → 一个不满足就失败
        }
        return true;
    }
};
