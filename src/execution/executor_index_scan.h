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
    IxIndexHandle *ih_ = nullptr;
    bool force_empty_ = false;

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
            auto ih_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
            auto ih_it = sm_manager_->ihs_.find(ih_name);
            if (ih_it != sm_manager_->ihs_.end()) {
                ih_ = ih_it->second.get();
            }
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
        if (ih_ == nullptr || index_col_names_.empty()) {
            scan_ = std::make_unique<RmScan>(fh_);
        } else {
            Iid lower = ih_->leaf_begin();
            Iid upper = ih_->leaf_end();
            if (!build_index_range(lower, upper) || force_empty_) {
                scan_ = std::make_unique<IxScan>(ih_, upper, upper, sm_manager_->get_bpm());
            } else {
                scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
            }
        }
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

    static std::string raw_key_string(const char *data, int len) {
        return std::string(data, data + len);
    }

    static Value min_value_for_col(const ColMeta &col) {
        Value v;
        switch (col.type) {
            case TYPE_INT:
                v = Value::make_int(std::numeric_limits<int>::min());
                break;
            case TYPE_BIGINT:
                v = Value::make_bigint(std::numeric_limits<int64_t>::min());
                break;
            case TYPE_FLOAT:
                v = Value::make_float(std::numeric_limits<float>::lowest());
                break;
            case TYPE_STRING:
                v = Value::make_string(std::string());
                break;
            case TYPE_DATETIME:
                v = Value::make_datetime(datetime_str_to_int64("1000-01-01 00:00:00"));
                break;
            default:
                throw InternalError("Unexpected index column type");
        }
        return v;
    }

    static Value max_value_for_col(const ColMeta &col) {
        Value v;
        switch (col.type) {
            case TYPE_INT:
                v = Value::make_int(std::numeric_limits<int>::max());
                break;
            case TYPE_BIGINT:
                v = Value::make_bigint(std::numeric_limits<int64_t>::max());
                break;
            case TYPE_FLOAT:
                v = Value::make_float(std::numeric_limits<float>::max());
                break;
            case TYPE_STRING:
                v = Value::make_string(std::string(col.len, static_cast<char>(0xFF)));
                break;
            case TYPE_DATETIME:
                v = Value::make_datetime(datetime_str_to_int64("9999-12-31 23:59:59"));
                break;
            default:
                throw InternalError("Unexpected index column type");
        }
        return v;
    }

    static void encode_value_to_key(char *dst, const ColMeta &col, const Value &value) {
        Value casted = TypeCaster::cast_value(value, col.type, col.len);
        casted.init_raw(col.len);
        memcpy(dst, casted.raw->data, col.len);
    }

    static bool better_lower(const BoundSpec &spec, const Value &candidate, bool inclusive) {
        if (!spec.has_lower) {
            return true;
        }
        int cmp = TypeCaster::compare_values(candidate, spec.lower);
        if (cmp > 0) {
            return true;
        }
        if (cmp == 0 && !inclusive && spec.lower_inclusive) {
            return true;
        }
        return false;
    }

    static bool better_upper(const BoundSpec &spec, const Value &candidate, bool inclusive) {
        if (!spec.has_upper) {
            return true;
        }
        int cmp = TypeCaster::compare_values(candidate, spec.upper);
        if (cmp < 0) {
            return true;
        }
        if (cmp == 0 && !inclusive && spec.upper_inclusive) {
            return true;
        }
        return false;
    }

    bool build_index_range(Iid &lower, Iid &upper) {
        force_empty_ = false;
        if (ih_ == nullptr || index_meta_.cols.empty()) {
            return false;
        }

        std::vector<BoundSpec> specs(index_meta_.cols.size());
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &col = index_meta_.cols[i];
            for (const auto &cond : conds_) {
                if (!cond.is_rhs_val) {
                    continue;
                }
                if (cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name) {
                    continue;
                }
                Value rhs = TypeCaster::cast_value(cond.rhs_val, col.type, col.len);
                if (cond.op == OP_EQ) {
                    if (specs[i].has_eq && TypeCaster::compare_values(specs[i].eq, rhs) != 0) {
                        force_empty_ = true;
                        return true;
                    }
                    specs[i].has_eq = true;
                    specs[i].eq = rhs;
                    continue;
                }
                if (cond.op == OP_GT || cond.op == OP_GE) {
                    bool inclusive = cond.op == OP_GE;
                    if (better_lower(specs[i], rhs, inclusive)) {
                        specs[i].has_lower = true;
                        specs[i].lower = rhs;
                        specs[i].lower_inclusive = inclusive;
                    }
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    bool inclusive = cond.op == OP_LE;
                    if (better_upper(specs[i], rhs, inclusive)) {
                        specs[i].has_upper = true;
                        specs[i].upper = rhs;
                        specs[i].upper_inclusive = inclusive;
                    }
                }
            }
            if (specs[i].has_eq) {
                if ((specs[i].has_lower && TypeCaster::compare_values(specs[i].eq, specs[i].lower) < 0) ||
                    (specs[i].has_upper && TypeCaster::compare_values(specs[i].eq, specs[i].upper) > 0)) {
                    force_empty_ = true;
                    return true;
                }
            } else if (specs[i].has_lower && specs[i].has_upper) {
                int cmp = TypeCaster::compare_values(specs[i].lower, specs[i].upper);
                if (cmp > 0 || (cmp == 0 && (!specs[i].lower_inclusive || !specs[i].upper_inclusive))) {
                    force_empty_ = true;
                    return true;
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

        std::vector<char> low_key(index_meta_.col_tot_len, 0);
        std::vector<char> high_key(index_meta_.col_tot_len, 0);
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &col = index_meta_.cols[i];
            if (!range_used && i < prefix) {
                encode_value_to_key(low_key.data() + offset, col, specs[i].eq);
                encode_value_to_key(high_key.data() + offset, col, specs[i].eq);
            } else if (range_used && i < range_idx) {
                encode_value_to_key(low_key.data() + offset, col, specs[i].eq);
                encode_value_to_key(high_key.data() + offset, col, specs[i].eq);
            } else if (range_used && i == range_idx) {
                encode_value_to_key(low_key.data() + offset, col,
                                    specs[i].has_lower ? specs[i].lower : min_value_for_col(col));
                encode_value_to_key(high_key.data() + offset, col,
                                    specs[i].has_upper ? specs[i].upper : max_value_for_col(col));
            } else {
                Value low_tail = min_value_for_col(col);
                Value high_tail = max_value_for_col(col);
                if (range_used && specs[range_idx].has_lower && !specs[range_idx].lower_inclusive) {
                    low_tail = max_value_for_col(col);
                }
                if (range_used && specs[range_idx].has_upper && !specs[range_idx].upper_inclusive) {
                    high_tail = min_value_for_col(col);
                }
                encode_value_to_key(low_key.data() + offset, col, low_tail);
                encode_value_to_key(high_key.data() + offset, col, high_tail);
            }
            offset += col.len;
        }

        bool has_lower = !range_used || specs[range_idx].has_lower || prefix > 0;
        bool has_upper = !range_used || specs[range_idx].has_upper || prefix > 0;
        bool lower_inclusive = !range_used || !specs[range_idx].has_lower || specs[range_idx].lower_inclusive;
        bool upper_inclusive = !range_used || !specs[range_idx].has_upper || specs[range_idx].upper_inclusive;

        lower = has_lower
                    ? (lower_inclusive ? ih_->lower_bound(low_key.data()) : ih_->upper_bound(low_key.data()))
                    : ih_->leaf_begin();
        upper = has_upper
                    ? (upper_inclusive ? ih_->upper_bound(high_key.data()) : ih_->lower_bound(high_key.data()))
                    : ih_->leaf_end();
        return true;
    }

    /** @brief 从 scan_ 当前位置推进到第一条满足 conds_ 条件的记录 */
    void advance_to_match() {
        while (!scan_->is_end()) {
            if (conds_.empty()) break;
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(rec->data, conds_, cols_)) break;
            scan_->next();
        }
        if (!scan_->is_end()) {
            rid_ = scan_->rid();
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

            int cmp = 0;
            if (cond.is_rhs_val) {
                cmp = TypeCaster::compare_raw_with_value(lhs_val, lhs_type, len, cond.rhs_val);
            } else {
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
            if (!ok) return false;
        }
        return true;
    }
};
