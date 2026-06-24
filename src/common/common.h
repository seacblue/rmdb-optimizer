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
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "defs.h"
#include "datetime_utils.h"
#include "errors.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        int64_t bigint_val;  // bigint/datetime value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    Value() : type(TYPE_INT), int_val(0) {}

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
        int_val = int_val_;
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        bigint_val = datetime_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    int as_int() const { return int_val; }

    int64_t as_bigint() const { return bigint_val; }

    int64_t as_datetime() const { return bigint_val; }

    float as_float() const { return float_val; }

    const std::string &as_string() const { return str_val; }

    bool is_numeric() const {
        return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
    }

    bool is_integer() const {
        return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_DATETIME;
    }

    long double as_numeric() const {
        switch (type) {
            case TYPE_INT:
                return static_cast<long double>(int_val);
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return static_cast<long double>(bigint_val);
            case TYPE_FLOAT:
                return static_cast<long double>(float_val);
            default:
                throw std::logic_error("Value is not numeric");
        }
    }

    int64_t as_integer() const {
        switch (type) {
            case TYPE_INT:
                return static_cast<int64_t>(int_val);
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return bigint_val;
            default:
                throw std::logic_error("Value is not integer");
        }
    }

    std::string debug_string() const {
        switch (type) {
            case TYPE_INT:
                return std::to_string(int_val);
            case TYPE_BIGINT:
                return std::to_string(bigint_val);
            case TYPE_DATETIME:
                return int64_to_datetime_str(bigint_val);
            case TYPE_FLOAT:
                return std::to_string(float_val);
            case TYPE_STRING:
                return str_val;
            default:
                return "<unknown>";
        }
    }

    void write_raw(char *dest, int len) const {
        if (type == TYPE_INT) {
            assert(len == static_cast<int>(sizeof(int)));
            *(int *)(dest) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == static_cast<int>(sizeof(float)));
            *(float *)(dest) = float_val;
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            assert(len == static_cast<int>(sizeof(int64_t)));
            *(int64_t *)(dest) = bigint_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(dest, 0, len);
            memcpy(dest, str_val.c_str(), str_val.size());
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

inline bool is_numeric_type(ColType type) {
    return type == TYPE_INT || type == TYPE_FLOAT || type == TYPE_BIGINT;
}

inline Value parse_integer_literal(const std::string &literal) {
    errno = 0;
    char *end = nullptr;
    long long parsed = std::strtoll(literal.c_str(), &end, 10);
    if (errno == ERANGE || end == literal.c_str() || end == nullptr || *end != '\0') {
        throw InvalidIntegerLiteralError(literal);
    }

    Value value;
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        value.set_bigint(static_cast<int64_t>(parsed));
    } else {
        value.set_int(static_cast<int>(parsed));
    }
    return value;
}

inline void coerce_value(Value &value, ColType target_type, int target_len) {
    if (target_type == TYPE_DATETIME) {
        if (value.type == TYPE_DATETIME) {
            value.init_raw(target_len);
            return;
        }
        if (value.type != TYPE_STRING) {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
        }
        if (!validate_datetime_str(value.str_val)) {
            throw InvalidDatetimeError(value.str_val);
        }
        value.set_datetime(datetime_str_to_int64(value.str_val));
        value.init_raw(target_len);
        return;
    }

    if (target_type == TYPE_STRING) {
        if (value.type != TYPE_STRING) {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
        }
        value.init_raw(target_len);
        return;
    }

    if (!is_numeric_type(target_type) || !value.is_numeric()) {
        if (target_type != value.type) {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
        }
        value.init_raw(target_len);
        return;
    }

    long double numeric = value.as_numeric();
    if (target_type == TYPE_INT) {
        if (!std::isfinite(static_cast<double>(numeric)) ||
            numeric < std::numeric_limits<int>::min() ||
            numeric > std::numeric_limits<int>::max()) {
            throw NumericOverflowError(coltype2str(target_type), value.debug_string());
        }
        value.set_int(static_cast<int>(numeric));
    } else if (target_type == TYPE_BIGINT) {
        if (!std::isfinite(static_cast<double>(numeric)) ||
            numeric < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
            numeric > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
            throw NumericOverflowError(coltype2str(target_type), value.debug_string());
        }
        value.set_bigint(static_cast<int64_t>(numeric));
    } else if (target_type == TYPE_FLOAT) {
        if (!std::isfinite(static_cast<double>(numeric))) {
            throw NumericOverflowError(coltype2str(target_type), value.debug_string());
        }
        value.set_float(static_cast<float>(numeric));
    }
    value.init_raw(target_len);
}

inline int compare_values(const Value &lhs, const Value &rhs) {
    if (lhs.is_numeric() && rhs.is_numeric()) {
        if (lhs.type == TYPE_FLOAT || rhs.type == TYPE_FLOAT) {
            long double l = lhs.as_numeric();
            long double r = rhs.as_numeric();
            long double diff = l - r;
            if (std::fabs(diff) < 1e-9L) {
                return 0;
            }
            return diff < 0 ? -1 : 1;
        }
        int64_t l = lhs.as_integer();
        int64_t r = rhs.as_integer();
        return l < r ? -1 : (l > r ? 1 : 0);
    }
    if (lhs.type == TYPE_STRING && rhs.type == TYPE_STRING) {
        return lhs.str_val.compare(rhs.str_val);
    }
    if (lhs.type == TYPE_DATETIME && rhs.type == TYPE_DATETIME) {
        return lhs.bigint_val < rhs.bigint_val ? -1 : (lhs.bigint_val > rhs.bigint_val ? 1 : 0);
    }
    throw IncompatibleTypeError(coltype2str(lhs.type), coltype2str(rhs.type));
}

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

inline bool compare_by_op(int cmp_result, CompOp op) {
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
};

struct AggregateDesc {
    AggType type;
    TabCol col;
    bool is_star = false;
    std::string alias;
};
