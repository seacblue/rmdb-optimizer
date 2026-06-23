#pragma once

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "common/common.h"

class TypeCaster {
   public:
    static bool is_numeric(ColType type) {
        return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
    }

    static bool is_integer(ColType type) {
        return type == TYPE_INT || type == TYPE_BIGINT;
    }

    static int cast_priority(ColType type) {
        switch (type) {
            case TYPE_INT:
                return 0;
            case TYPE_BIGINT:
                return 1;
            case TYPE_FLOAT:
                return 2;
            default:
                return -1;
        }
    }

    static int storage_size(ColType type, int len) {
        switch (type) {
            case TYPE_INT:
                return sizeof(int);
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return sizeof(int64_t);
            case TYPE_FLOAT:
                return sizeof(float);
            case TYPE_STRING:
                return len;
            default:
                throw InternalError("Unexpected column type");
        }
    }

    static ColType common_numeric_type(ColType lhs, ColType rhs) {
        if (!is_numeric(lhs) || !is_numeric(rhs)) {
            throw IncompatibleTypeError(coltype2str(lhs), coltype2str(rhs));
        }
        if (lhs == TYPE_FLOAT || rhs == TYPE_FLOAT) {
            return TYPE_FLOAT;
        }
        if (lhs == TYPE_BIGINT || rhs == TYPE_BIGINT) {
            return TYPE_BIGINT;
        }
        return TYPE_INT;
    }

    static bool can_cast(ColType from, ColType to) {
        if (from == to) {
            return true;
        }
        if (is_numeric(from) && is_numeric(to)) {
            return true;
        }
        if (from == TYPE_STRING && to == TYPE_DATETIME) {
            return true;
        }
        return false;
    }

    static Value parse_integer_literal(const std::string &literal) {
        errno = 0;
        char *end = nullptr;
        long long parsed = std::strtoll(literal.c_str(), &end, 10);
        if (errno == ERANGE || end == literal.c_str() || end == nullptr || *end != '\0') {
            throw InvalidIntegerLiteralError(literal);
        }
        return make_integer_value(static_cast<int64_t>(parsed));
    }

    static Value make_integer_value(int64_t value, ColType preferred_type = TYPE_INT) {
        if (preferred_type == TYPE_BIGINT) {
            return Value::make_bigint(value);
        }
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            return Value::make_bigint(value);
        }
        return Value::make_int(static_cast<int>(value));
    }

    static Value cast_value(const Value &value, ColType target_type, int target_len = 0) {
        switch (target_type) {
            case TYPE_INT: {
                if (!is_numeric(value.type)) {
                    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
                }
                long double numeric = value.as_numeric();
                if (!std::isfinite(static_cast<double>(numeric)) ||
                    numeric < std::numeric_limits<int>::min() ||
                    numeric > std::numeric_limits<int>::max()) {
                    throw NumericOverflowError(coltype2str(target_type), value.debug_string());
                }
                return Value::make_int(static_cast<int>(numeric));
            }
            case TYPE_BIGINT: {
                if (!is_numeric(value.type)) {
                    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
                }
                long double numeric = value.as_numeric();
                if (!std::isfinite(static_cast<double>(numeric)) ||
                    numeric < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
                    numeric > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
                    throw NumericOverflowError(coltype2str(target_type), value.debug_string());
                }
                return Value::make_bigint(static_cast<int64_t>(numeric));
            }
            case TYPE_FLOAT: {
                if (!is_numeric(value.type)) {
                    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
                }
                long double numeric = value.as_numeric();
                if (!std::isfinite(static_cast<double>(numeric))) {
                    throw NumericOverflowError(coltype2str(target_type), value.debug_string());
                }
                return Value::make_float(static_cast<float>(numeric));
            }
            case TYPE_STRING: {
                if (value.type != TYPE_STRING) {
                    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
                }
                if (target_len > 0 && static_cast<int>(value.as_string().size()) > target_len) {
                    throw StringOverflowError();
                }
                return Value::make_string(value.as_string());
            }
            case TYPE_DATETIME: {
                if (value.type == TYPE_DATETIME) {
                    return Value::make_datetime(value.as_datetime());
                }
                if (value.type != TYPE_STRING) {
                    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
                }
                if (!validate_datetime_str(value.as_string())) {
                    throw InvalidDatetimeError(value.as_string());
                }
                return Value::make_datetime(datetime_str_to_int64(value.as_string()));
            }
            default:
                throw InternalError("Unexpected target type");
        }
    }

    static Value read_raw_value(const char *data, ColType type, int len) {
        return Value::from_raw(type, data, len);
    }

    static int compare_values(const Value &lhs, const Value &rhs) {
        if (is_numeric(lhs.type) && is_numeric(rhs.type)) {
            ColType common = common_numeric_type(lhs.type, rhs.type);
            if (common == TYPE_FLOAT) {
                long double lhs_num = lhs.as_numeric();
                long double rhs_num = rhs.as_numeric();
                if (std::abs(lhs_num - rhs_num) < 1e-9L) {
                    return 0;
                }
                return lhs_num < rhs_num ? -1 : 1;
            }
            int64_t lhs_num = cast_value(lhs, TYPE_BIGINT).as_integer();
            int64_t rhs_num = cast_value(rhs, TYPE_BIGINT).as_integer();
            return lhs_num < rhs_num ? -1 : (lhs_num > rhs_num ? 1 : 0);
        }

        if (lhs.type == TYPE_DATETIME && rhs.type == TYPE_STRING) {
            return compare_values(lhs, cast_value(rhs, TYPE_DATETIME));
        }
        if (lhs.type == TYPE_STRING && rhs.type == TYPE_DATETIME) {
            return compare_values(cast_value(lhs, TYPE_DATETIME), rhs);
        }
        if (lhs.type == TYPE_DATETIME && rhs.type == TYPE_DATETIME) {
            int64_t lhs_dt = lhs.as_datetime();
            int64_t rhs_dt = rhs.as_datetime();
            return lhs_dt < rhs_dt ? -1 : (lhs_dt > rhs_dt ? 1 : 0);
        }
        if (lhs.type == TYPE_STRING && rhs.type == TYPE_STRING) {
            return lhs.as_string().compare(rhs.as_string());
        }
        throw IncompatibleTypeError(coltype2str(lhs.type), coltype2str(rhs.type));
    }

    static int compare_raw(const char *lhs_data, ColType lhs_type, int lhs_len,
                           const char *rhs_data, ColType rhs_type, int rhs_len) {
        Value lhs = read_raw_value(lhs_data, lhs_type, lhs_len);
        Value rhs = read_raw_value(rhs_data, rhs_type, rhs_len);
        return compare_values(lhs, rhs);
    }

    static int compare_raw_with_value(const char *lhs_data, ColType lhs_type, int lhs_len, const Value &rhs) {
        Value lhs = read_raw_value(lhs_data, lhs_type, lhs_len);
        return compare_values(lhs, rhs);
    }

    static std::string format_raw_value(const char *data, ColType type, int len) {
        return Value::from_raw(type, data, len).debug_string();
    }

    static Value negate_value(const Value &value) {
        if (value.type == TYPE_FLOAT) {
            return Value::make_float(-value.as_float());
        }
        if (!is_integer(value.type)) {
            throw IncompatibleTypeError(coltype2str(value.type), "NUMERIC");
        }
        int64_t numeric = value.as_integer();
        if (numeric == std::numeric_limits<int64_t>::min()) {
            throw NumericOverflowError("BIGINT", value.debug_string());
        }
        return make_integer_value(-numeric, value.type == TYPE_BIGINT ? TYPE_BIGINT : TYPE_INT);
    }

    static Value apply_arithmetic(const Value &lhs, const Value &rhs, char op) {
        if (!is_numeric(lhs.type) || !is_numeric(rhs.type)) {
            throw IncompatibleTypeError(coltype2str(lhs.type), coltype2str(rhs.type));
        }

        ColType common = common_numeric_type(lhs.type, rhs.type);
        if (common == TYPE_FLOAT) {
            long double lhs_num = lhs.as_numeric();
            long double rhs_num = rhs.as_numeric();
            if (op == '/' && std::abs(rhs_num) < 1e-12L) {
                throw InternalError("Division by zero in update expression");
            }
            long double result_num = 0.0;
            switch (op) {
                case '+':
                    result_num = lhs_num + rhs_num;
                    break;
                case '-':
                    result_num = lhs_num - rhs_num;
                    break;
                case '*':
                    result_num = lhs_num * rhs_num;
                    break;
                case '/':
                    result_num = lhs_num / rhs_num;
                    break;
                default:
                    throw InternalError("Unexpected arithmetic operator");
            }
            Value result;
            return Value::make_float(static_cast<float>(result_num));
        }

        int64_t lhs_num = cast_value(lhs, TYPE_BIGINT).as_integer();
        int64_t rhs_num = cast_value(rhs, TYPE_BIGINT).as_integer();
        if (op == '/' && rhs_num == 0) {
            throw InternalError("Division by zero in update expression");
        }
        int64_t result_num = 0;
        switch (op) {
            case '+':
                if (__builtin_add_overflow(lhs_num, rhs_num, &result_num)) {
                    throw NumericOverflowError("BIGINT", lhs.debug_string() + " + " + rhs.debug_string());
                }
                break;
            case '-':
                if (__builtin_sub_overflow(lhs_num, rhs_num, &result_num)) {
                    throw NumericOverflowError("BIGINT", lhs.debug_string() + " - " + rhs.debug_string());
                }
                break;
            case '*':
                if (__builtin_mul_overflow(lhs_num, rhs_num, &result_num)) {
                    throw NumericOverflowError("BIGINT", lhs.debug_string() + " * " + rhs.debug_string());
                }
                break;
            case '/':
                if (lhs_num == std::numeric_limits<int64_t>::min() && rhs_num == -1) {
                    throw NumericOverflowError("BIGINT", lhs.debug_string() + " / " + rhs.debug_string());
                }
                result_num = lhs_num / rhs_num;
                break;
            default:
                throw InternalError("Unexpected arithmetic operator");
        }
        return make_integer_value(result_num, common == TYPE_BIGINT ? TYPE_BIGINT : TYPE_INT);
    }
};
