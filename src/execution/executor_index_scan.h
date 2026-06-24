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
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<std::string> index_col_names_;
    std::shared_ptr<MemoryIndex> mem_index_;

    std::vector<Rid> matched_rids_;
    size_t cursor_ = 0;
    Rid rid_;
    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context)
        : tab_name_(std::move(tab_name)),
          conds_(std::move(conds)),
          index_col_names_(std::move(index_col_names)),
          sm_manager_(sm_manager) {
        context_ = context;
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        if (!index_col_names_.empty()) {
            try {
                mem_index_ = sm_manager_->get_memory_index(tab_name_, index_col_names_);
            } catch (...) {
                mem_index_.reset();
            }
        }
    }

    void beginTuple() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        matched_rids_.clear();
        cursor_ = 0;
        if (mem_index_ != nullptr && collect_index_candidates()) {
            filter_candidates();
        } else {
            collect_full_scan_candidates();
        }
        if (!matched_rids_.empty()) {
            rid_ = matched_rids_[0];
        }
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        ++cursor_;
        if (!is_end()) {
            rid_ = matched_rids_[cursor_];
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return cursor_ >= matched_rids_.size(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    struct BoundSpec {
        bool has_eq = false;
        Value eq;
        bool has_lower = false;
        Value lower;
        bool lower_inclusive = true;
        bool has_upper = false;
        Value upper;
        bool upper_inclusive = true;
    };

    static Value min_value_for_col(const ColMeta &col) {
        switch (col.type) {
            case TYPE_INT:
                return Value::make_int(std::numeric_limits<int>::min());
            case TYPE_BIGINT:
                return Value::make_bigint(std::numeric_limits<int64_t>::min());
            case TYPE_FLOAT:
                return Value::make_float(std::numeric_limits<float>::lowest());
            case TYPE_STRING:
                return Value::make_string(std::string());
            case TYPE_DATETIME:
                return Value::make_datetime(datetime_str_to_int64("1000-01-01 00:00:00"));
            default:
                throw InternalError("Unexpected index column type");
        }
    }

    static Value max_value_for_col(const ColMeta &col) {
        switch (col.type) {
            case TYPE_INT:
                return Value::make_int(std::numeric_limits<int>::max());
            case TYPE_BIGINT:
                return Value::make_bigint(std::numeric_limits<int64_t>::max());
            case TYPE_FLOAT:
                return Value::make_float(std::numeric_limits<float>::max());
            case TYPE_STRING:
                return Value::make_string(std::string(col.len, static_cast<char>(0xFF)));
            case TYPE_DATETIME:
                return Value::make_datetime(datetime_str_to_int64("9999-12-31 23:59:59"));
            default:
                throw InternalError("Unexpected index column type");
        }
    }

    static void encode_value_to_key(std::string &buf, int offset, const ColMeta &col, const Value &value) {
        Value casted = value;
        coerce_value(casted, col.type, col.len);
        memcpy(buf.data() + offset, casted.raw->data, col.len);
    }

    static bool eval_conds(const char *rec_data, const std::vector<Condition> &conds,
                           const std::vector<ColMeta> &cols) {
        for (const auto &cond : conds) {
            auto lhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                return c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name;
            });
            if (lhs_it == cols.end()) return false;
            Value lhs = Value::from_raw(lhs_it->type, rec_data + lhs_it->offset, lhs_it->len);
            int cmp = 0;
            if (cond.is_rhs_val) {
                cmp = compare_values(lhs, cond.rhs_val);
            } else {
                auto rhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &c) {
                    return c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name;
                });
                if (rhs_it == cols.end()) return false;
                Value rhs = Value::from_raw(rhs_it->type, rec_data + rhs_it->offset, rhs_it->len);
                cmp = compare_values(lhs, rhs);
            }
            if (!compare_by_op(cmp, cond.op)) {
                return false;
            }
        }
        return true;
    }

    void collect_full_scan_candidates() {
        RmScan scan(fh_);
        while (!scan.is_end()) {
            Rid candidate = scan.rid();
            auto rec = fh_->get_record(candidate, context_);
            if (eval_conds(rec->data, conds_, cols_)) {
                matched_rids_.push_back(candidate);
            }
            scan.next();
        }
    }

    bool collect_index_candidates() {
        if (mem_index_ == nullptr) {
            return false;
        }

        std::vector<BoundSpec> specs(mem_index_->meta.cols.size());
        for (size_t i = 0; i < mem_index_->meta.cols.size(); ++i) {
            const auto &col = mem_index_->meta.cols[i];
            for (const auto &cond : conds_) {
                if (!cond.is_rhs_val) {
                    continue;
                }
                if (cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name) {
                    continue;
                }
                Value rhs = cond.rhs_val;
                coerce_value(rhs, col.type, col.len);
                if (cond.op == OP_EQ) {
                    specs[i].has_eq = true;
                    specs[i].eq = rhs;
                    continue;
                }
                if (cond.op == OP_GT || cond.op == OP_GE) {
                    specs[i].has_lower = true;
                    specs[i].lower = rhs;
                    specs[i].lower_inclusive = (cond.op == OP_GE);
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    specs[i].has_upper = true;
                    specs[i].upper = rhs;
                    specs[i].upper_inclusive = (cond.op == OP_LE);
                }
            }
        }

        size_t prefix = 0;
        bool range_used = false;
        size_t range_idx = 0;
        for (size_t i = 0; i < specs.size(); ++i) {
            if (specs[i].has_eq) {
                ++prefix;
                continue;
            }
            if (specs[i].has_lower || specs[i].has_upper) {
                ++prefix;
                range_used = true;
                range_idx = i;
            }
            break;
        }
        if (prefix == 0) {
            return false;
        }

        std::string low_key(mem_index_->meta.col_tot_len, '\0');
        std::string high_key(mem_index_->meta.col_tot_len, '\0');
        int offset = 0;
        for (size_t i = 0; i < mem_index_->meta.cols.size(); ++i) {
            const auto &col = mem_index_->meta.cols[i];
            if (!range_used && i < prefix) {
                encode_value_to_key(low_key, offset, col, specs[i].eq);
                encode_value_to_key(high_key, offset, col, specs[i].eq);
            } else if (range_used && i < range_idx) {
                encode_value_to_key(low_key, offset, col, specs[i].eq);
                encode_value_to_key(high_key, offset, col, specs[i].eq);
            } else if (range_used && i == range_idx) {
                encode_value_to_key(low_key, offset, col,
                                    specs[i].has_lower ? specs[i].lower : min_value_for_col(col));
                encode_value_to_key(high_key, offset, col,
                                    specs[i].has_upper ? specs[i].upper : max_value_for_col(col));
            } else {
                encode_value_to_key(low_key, offset, col, min_value_for_col(col));
                encode_value_to_key(high_key, offset, col, max_value_for_col(col));
            }
            offset += col.len;
        }

        auto begin_it = mem_index_->entries.begin();
        auto end_it = mem_index_->entries.end();
        if (!range_used || specs[range_idx].has_lower || prefix > 0) {
            if (range_used && specs[range_idx].has_lower && !specs[range_idx].lower_inclusive) {
                begin_it = mem_index_->entries.upper_bound(low_key);
            } else {
                begin_it = mem_index_->entries.lower_bound(low_key);
            }
        }
        if (!range_used || specs[range_idx].has_upper || prefix > 0) {
            if (range_used && specs[range_idx].has_upper && !specs[range_idx].upper_inclusive) {
                end_it = mem_index_->entries.lower_bound(high_key);
            } else {
                end_it = mem_index_->entries.upper_bound(high_key);
            }
        }

        for (auto it = begin_it; it != end_it; ++it) {
            matched_rids_.push_back(it->second);
        }
        return true;
    }

    void filter_candidates() {
        std::vector<Rid> filtered;
        filtered.reserve(matched_rids_.size());
        for (const auto &candidate : matched_rids_) {
            auto rec = fh_->get_record(candidate, context_);
            if (eval_conds(rec->data, conds_, cols_)) {
                filtered.push_back(candidate);
            }
        }
        matched_rids_.swap(filtered);
    }
};
