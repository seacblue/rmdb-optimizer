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
#include <iostream>
#include <iomanip>
#include <ostream>
#include <string>
#include <sstream>
#include <vector>
#include "common/context.h"
#include "common/config.h"

#define RECORD_COUNT_LENGTH 40

class RecordPrinter {
    static constexpr size_t COL_WIDTH = 16;
    size_t num_cols;
public:
    RecordPrinter(size_t num_cols_) : num_cols(num_cols_) {
        assert(num_cols_ > 0);
    }

    std::string render_separator() const {
        std::string out;
        for (size_t i = 0; i < num_cols; i++) {
            out += "+" + std::string(COL_WIDTH + 2, '-');
        }
        out += "+\n";
        return out;
    }

    std::string render_record(const std::vector<std::string> &rec_str) const {
        assert(rec_str.size() == num_cols);
        std::string out;
        for (auto col : rec_str) {
            if (col.size() > COL_WIDTH) {
                col = col.substr(0, COL_WIDTH - 3) + "...";
            }
            std::stringstream ss;
            ss << "| " << std::setw(COL_WIDTH) << col << " ";
            out += ss.str();
        }
        out += "|\n";
        return out;
    }

    static std::string render_record_count(size_t num_rec, bool ellipsis = false) {
        std::string out;
        if (ellipsis) {
            out += "... ...\n";
        }
        out += "Total record(s): " + std::to_string(num_rec) + '\n';
        return out;
    }

    void print_separator(std::ostream &os) const {
        os << render_separator();
    }

    void print_record(const std::vector<std::string> &rec_str, std::ostream &os) const {
        os << render_record(rec_str);
    }

    static void print_record_count(size_t num_rec, std::ostream &os, bool ellipsis = false) {
        os << render_record_count(num_rec, ellipsis);
    }

    void print_separator(Context *context) const {
        auto str = render_separator();
        if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
            *(context->offset_) = *(context->offset_) + str.length();
        }
        else {
            context->ellipsis_ = true;
        }
    }

    void print_record(const std::vector<std::string> &rec_str, Context *context) const {
        auto str = render_record(rec_str);
        if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
            *(context->offset_) = *(context->offset_) + str.length();
        }
    }

    static void print_record_count(size_t num_rec, Context *context) {
        std::string str = render_record_count(num_rec, context->ellipsis_);
        memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
        *(context->offset_) = *(context->offset_) + str.length();
    }
};
