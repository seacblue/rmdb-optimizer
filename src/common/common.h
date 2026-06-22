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
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

// ==================== DATETIME 工具函数 ====================
// 编码格式: int64_t = YYYYMMDDHHMMSS (14 位十进制数)
// 比较直接使用 int64_t 即可保持字典序一致

inline bool validate_datetime_str(const std::string &str) {
    // 格式: YYYY-MM-DD HH:MM:SS  (19 字符)
    if (str.length() != 19) return false;
    if (str[4] != '-' || str[7] != '-' || str[10] != ' ' || str[13] != ':' || str[16] != ':') return false;
    for (int i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (!isdigit(str[i])) return false;
    }
    int year  = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day   = std::stoi(str.substr(8, 2));
    int hour  = std::stoi(str.substr(11, 2));
    int min   = std::stoi(str.substr(14, 2));
    int sec   = std::stoi(str.substr(17, 2));
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    if (hour < 0 || hour > 23) return false;
    if (min < 0 || min > 59) return false;
    if (sec < 0 || sec > 59) return false;
    // 按月份校验天数
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)) days_in_month[1] = 29;
    if (day > days_in_month[month - 1]) return false;
    return true;
}

inline int64_t datetime_str_to_int64(const std::string &str) {
    int year  = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day   = std::stoi(str.substr(8, 2));
    int hour  = std::stoi(str.substr(11, 2));
    int min   = std::stoi(str.substr(14, 2));
    int sec   = std::stoi(str.substr(17, 2));
    return (int64_t)year * 10000000000LL + (int64_t)month * 100000000LL +
           (int64_t)day * 1000000LL + (int64_t)hour * 10000LL +
           (int64_t)min * 100LL + (int64_t)sec;
}

inline std::string int64_to_datetime_str(int64_t val) {
    int sec   = val % 100; val /= 100;
    int min   = val % 100; val /= 100;
    int hour  = val % 100; val /= 100;
    int day   = val % 100; val /= 100;
    int month = val % 100; val /= 100;
    int year  = static_cast<int>(val);
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);
    return std::string(buf);
}

// ============================================================

struct Value {
    ColType type;  // type of value
    union {
        int int_val;        // int value
        float float_val;    // float value
        int64_t bigint_val; // bigint value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

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

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
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
};