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
#include <climits>
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

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        // RmScan() constructor already calls next(), so we're at first record.
        // Advance to first record matching all conditions (if any).
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
            std::unique_ptr<char[]> rhs_buf;
            const char *rhs_val = nullptr;
            if (cond.is_rhs_val) {
                rhs_buf = std::make_unique<char[]>(len);
                memset(rhs_buf.get(), 0, len);
                if (cond.rhs_val.type == TYPE_INT) {
                    if (type == TYPE_BIGINT) {
                        // Implicit widen int → bigint
                        *(int64_t *)rhs_buf.get() = static_cast<int64_t>(cond.rhs_val.int_val);
                    } else {
                        *(int *)rhs_buf.get() = cond.rhs_val.int_val;
                    }
                } else if (cond.rhs_val.type == TYPE_BIGINT) {
                    if (type == TYPE_INT) {
                        // Implicit narrow bigint → int (if value fits)
                        int64_t v = cond.rhs_val.bigint_val;
                        if (v > INT_MAX || v < INT_MIN) {
                            return false;  // overflow, condition fails
                        }
                        *(int *)rhs_buf.get() = static_cast<int>(v);
                    } else {
                        *(int64_t *)rhs_buf.get() = cond.rhs_val.bigint_val;
                    }
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    *(float *)rhs_buf.get() = cond.rhs_val.float_val;
                } else if (cond.rhs_val.type == TYPE_DATETIME) {
                    *(int64_t *)rhs_buf.get() = cond.rhs_val.bigint_val;
                } else {
                    memcpy(rhs_buf.get(), cond.rhs_val.str_val.c_str(),
                           std::min((int)cond.rhs_val.str_val.size(), len));
                }
                rhs_val = rhs_buf.get();
            } else {
                // 右值也是列（列-列比较，仅在同一张表中支持）
                auto rhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                    return c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name;
                });
                if (rhs_it == cols.end()) return false;
                ColType rhs_type = rhs_it->type;
                rhs_val = rec_data + rhs_it->offset;
            }

            // 比较
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
            if (!ok) return false;   // AND → 一个不满足就失败
        }
        return true;
    }

    /** @brief 通用值比较 */
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
        // TYPE_STRING
        return strncmp(a, b, len);
    }
};
