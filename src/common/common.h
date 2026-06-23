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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <variant>
#include <vector>
#include "defs.h"
#include "datetime_utils.h"
#include "errors.h"
#include "record/rm_defs.h"

namespace ast {
struct Expr;
}

struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

// ============================================================

struct Value {
    ColType type = TYPE_INT;  // type of value
    using Storage = std::variant<std::monostate, int, int64_t, float, std::string>;
    Storage data_;

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    Value() = default;

    static Value make_int(int value) {
        Value v;
        v.set_int(value);
        return v;
    }

    static Value make_bigint(int64_t value) {
        Value v;
        v.set_bigint(value);
        return v;
    }

    static Value make_datetime(int64_t value) {
        Value v;
        v.set_datetime(value);
        return v;
    }

    static Value make_float(float value) {
        Value v;
        v.set_float(value);
        return v;
    }

    static Value make_string(std::string value) {
        Value v;
        v.set_str(std::move(value));
        return v;
    }

    void set_int(int int_val_) {
        type = TYPE_INT;
        data_ = int_val_;
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        data_ = bigint_val_;
    }

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        data_ = datetime_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        data_ = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        data_ = std::move(str_val_);
    }

    int as_int() const {
        return std::get<int>(data_);
    }

    int64_t as_bigint() const {
        return std::get<int64_t>(data_);
    }

    int64_t as_datetime() const {
        return std::get<int64_t>(data_);
    }

    float as_float() const {
        return std::get<float>(data_);
    }

    const std::string &as_string() const {
        return std::get<std::string>(data_);
    }

    bool holds_int() const {
        return std::holds_alternative<int>(data_);
    }

    bool holds_bigint_like() const {
        return std::holds_alternative<int64_t>(data_);
    }

    bool holds_float() const {
        return std::holds_alternative<float>(data_);
    }

    bool holds_string() const {
        return std::holds_alternative<std::string>(data_);
    }

    bool is_numeric() const {
        return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
    }

    bool is_integer() const {
        return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_DATETIME;
    }

    long double as_numeric() const {
        switch (type) {
            case TYPE_INT:
                return static_cast<long double>(as_int());
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return static_cast<long double>(as_bigint());
            case TYPE_FLOAT:
                return static_cast<long double>(as_float());
            default:
                throw std::logic_error("Value is not numeric");
        }
    }

    int64_t as_integer() const {
        switch (type) {
            case TYPE_INT:
                return static_cast<int64_t>(as_int());
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return as_bigint();
            default:
                throw std::logic_error("Value is not integer");
        }
    }

    std::string debug_string() const {
        switch (type) {
            case TYPE_INT:
                return std::to_string(as_int());
            case TYPE_BIGINT:
                return std::to_string(as_bigint());
            case TYPE_DATETIME:
                return int64_to_datetime_str(as_datetime());
            case TYPE_FLOAT:
                return std::to_string(as_float());
            case TYPE_STRING:
                return as_string();
            default:
                return "<unknown>";
        }
    }

    void write_raw(char *dest, int len) const {
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(dest) = as_int();
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(dest) = as_float();
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(dest) = as_bigint();
        } else if (type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(dest) = as_datetime();
        } else if (type == TYPE_STRING) {
            if (len < (int)as_string().size()) {
                throw StringOverflowError();
            }
            memset(dest, 0, len);
            memcpy(dest, as_string().c_str(), as_string().size());
        }
    }

    void init_raw(int len) {
        raw = std::make_shared<RmRecord>(len);
        write_raw(raw->data, len);
    }

    static Value from_raw(ColType type, const char *data, int len) {
        switch (type) {
            case TYPE_INT:
                return Value::make_int(*reinterpret_cast<const int *>(data));
            case TYPE_BIGINT:
                return Value::make_bigint(*reinterpret_cast<const int64_t *>(data));
            case TYPE_DATETIME:
                return Value::make_datetime(*reinterpret_cast<const int64_t *>(data));
            case TYPE_FLOAT:
                return Value::make_float(*reinterpret_cast<const float *>(data));
            case TYPE_STRING:
                return Value::make_string(std::string(data, strnlen(data, len)));
            default:
                throw InternalError("Unexpected raw value type");
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
    std::shared_ptr<ast::Expr> rhs_expr;
};
