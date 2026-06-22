/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file datetime_utils.h
 * @brief DATETIME 工具函数 — 与 common.h 解耦
 *
 * 编码格式: int64_t = YYYYMMDDHHMMSS (14 位十进制数)
 * 比较直接使用 int64_t 即可保持字典序一致
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ==================== DATETIME 校验 ====================
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

// ==================== DATETIME 编码 ====================
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

// ==================== DATETIME 解码 ====================
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
