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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names;
        if (!index_col_names_.empty()) {
            index_meta_ = *(tab_.get_index_meta(index_col_names_));
        }
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        // 当前 IxIndexHandle (B+ 树) 尚未完全实现，
        // 因此退化使用 RmScan 进行全表扫描 + 条件过滤。
        // 待 B+ 树的 insert_entry / leaf_lookup / IxScan 实现后，
        // 可将下方替换为 IIxScan 的范围扫描以利用索引加速。
        scan_ = std::make_unique<RmScan>(fh_);
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
    /** @brief 从 scan_ 当前位置推进到第一条满足 conds_ 条件的记录 */
    void advance_to_match() {
        while (!scan_->is_end()) {
            if (conds_.empty()) break;
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(rec->data, conds_, cols_)) break;
            scan_->next();
        }
    }

    /** @brief 求值一组条件（AND 语义） */
    static bool eval_conds(const char *rec_data, const std::vector<Condition> &conds,
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
