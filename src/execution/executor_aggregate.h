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

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<AggregateDesc> aggs_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    std::unique_ptr<RmRecord> result_;
    bool returned_ = false;

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggregateDesc> aggs)
        : prev_(std::move(prev)), aggs_(std::move(aggs)) {
        auto &child_cols = prev_->cols();
        size_t offset = 0;
        for (const auto &agg : aggs_) {
            ColMeta out_col;
            out_col.tab_name = "";
            out_col.name = agg.alias;
            out_col.offset = static_cast<int>(offset);
            out_col.index = false;

            if (agg.type == AGG_COUNT) {
                out_col.type = TYPE_INT;
                out_col.len = sizeof(int);
            } else {
                auto it = get_col(child_cols, agg.col);
                if (agg.type == AGG_SUM) {
                    out_col.type = (it->type == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_BIGINT;
                    out_col.len = (out_col.type == TYPE_FLOAT) ? sizeof(float) : sizeof(int64_t);
                } else {
                    out_col.type = it->type;
                    out_col.len = it->len;
                }
            }
            offset += out_col.len;
            cols_.push_back(out_col);
        }
        len_ = offset;
    }

    void beginTuple() override {
        returned_ = false;
        result_ = std::make_unique<RmRecord>(len_);
        std::vector<bool> initialized(aggs_.size(), false);
        std::vector<int64_t> int_acc(aggs_.size(), 0);
        std::vector<long double> float_acc(aggs_.size(), 0.0);
        std::vector<Value> agg_values(aggs_.size());

        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            for (size_t i = 0; i < aggs_.size(); ++i) {
                const auto &agg = aggs_[i];
                if (agg.type == AGG_COUNT) {
                    int_acc[i] += 1;
                    initialized[i] = true;
                    continue;
                }

                auto src_col = get_col(prev_->cols(), agg.col);
                Value current = Value::from_raw(src_col->type, rec->data + src_col->offset, src_col->len);
                if (agg.type == AGG_SUM) {
                    if (src_col->type == TYPE_FLOAT) {
                        float_acc[i] += current.as_numeric();
                    } else {
                        int_acc[i] += current.as_integer();
                    }
                    initialized[i] = true;
                } else if (!initialized[i]) {
                    agg_values[i] = current;
                    initialized[i] = true;
                } else {
                    int cmp = compare_values(current, agg_values[i]);
                    if ((agg.type == AGG_MAX && cmp > 0) || (agg.type == AGG_MIN && cmp < 0)) {
                        agg_values[i] = current;
                    }
                }
            }
            prev_->nextTuple();
        }

        for (size_t i = 0; i < aggs_.size(); ++i) {
            Value out;
            const auto &meta = cols_[i];
            if (aggs_[i].type == AGG_COUNT) {
                out.set_int(static_cast<int>(int_acc[i]));
            } else if (aggs_[i].type == AGG_SUM) {
                if (meta.type == TYPE_FLOAT) {
                    out.set_float(static_cast<float>(float_acc[i]));
                } else {
                    out.set_bigint(int_acc[i]);
                }
            } else if (initialized[i]) {
                out = agg_values[i];
            } else {
                if (meta.type == TYPE_FLOAT) out.set_float(0.0f);
                else if (meta.type == TYPE_STRING) out.set_str("");
                else if (meta.type == TYPE_BIGINT || meta.type == TYPE_DATETIME) out.set_bigint(0);
                else out.set_int(0);
            }
            coerce_value(out, meta.type, meta.len);
            memcpy(result_->data + meta.offset, out.raw->data, meta.len);
        }
    }

    void nextTuple() override { returned_ = true; }

    std::unique_ptr<RmRecord> Next() override {
        if (returned_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*result_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return returned_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
