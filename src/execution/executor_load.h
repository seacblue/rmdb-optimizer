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

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "execution_defs.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class LoadExecutor : public AbstractExecutor {
   private:
    SmManager *sm_manager_;
    std::string tab_name_;
    std::string file_path_;

    static std::vector<std::string> split_csv_line(const std::string &line) {
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }
        if (!line.empty() && line.back() == ',') {
            fields.emplace_back();
        }
        return fields;
    }

   public:
    LoadExecutor(SmManager *sm_manager, std::string tab_name, std::string file_path, Context *context)
        : sm_manager_(sm_manager), tab_name_(std::move(tab_name)), file_path_(std::move(file_path)) {
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        std::ifstream csv(file_path_);
        if (!csv.is_open()) {
            throw FileNotFoundError(file_path_);
        }

        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name_).get();

        std::string header_line;
        if (!std::getline(csv, header_line)) {
            return nullptr;
        }
        auto headers = split_csv_line(header_line);
        std::vector<ColMeta> ordered_cols;
        ordered_cols.reserve(headers.size());
        for (const auto &header : headers) {
            ordered_cols.push_back(*tab.get_col(header));
        }

        bool old_logging = enable_logging.exchange(false);
        auto restore_logging = [&]() { enable_logging.store(old_logging); };

        try {
            std::string line;
            int record_size = tab.cols.back().offset + tab.cols.back().len;
            while (std::getline(csv, line)) {
                if (line.empty()) {
                    continue;
                }
                auto fields = split_csv_line(line);
                if (fields.size() != ordered_cols.size()) {
                    throw InternalError("LOAD column count mismatch");
                }

                std::vector<char> record_buf(record_size, 0);
                for (size_t i = 0; i < fields.size(); ++i) {
                    const auto &col = ordered_cols[i];
                    char *dst = record_buf.data() + col.offset;
                    if (col.type == TYPE_INT) {
                        int value = std::stoi(fields[i]);
                        memcpy(dst, &value, sizeof(int));
                    } else if (col.type == TYPE_FLOAT) {
                        float value = std::stof(fields[i]);
                        memcpy(dst, &value, sizeof(float));
                    } else {
                        if (static_cast<int>(fields[i].size()) > col.len) {
                            throw StringOverflowError();
                        }
                        memcpy(dst, fields[i].data(), fields[i].size());
                    }
                }

                Rid rid = fh->insert_record(record_buf.data(), nullptr);
                for (const auto &index : tab.indexes) {
                    auto ih_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                    auto ih_it = sm_manager_->ihs_.find(ih_name);
                    if (ih_it == sm_manager_->ihs_.end()) {
                        continue;
                    }

                    std::vector<char> key_buf(index.col_tot_len, 0);
                    int offset = 0;
                    for (const auto &col : index.cols) {
                        memcpy(key_buf.data() + offset, record_buf.data() + col.offset, col.len);
                        offset += col.len;
                    }
                    ih_it->second->insert_entry(key_buf.data(), rid, context_ != nullptr ? context_->txn_ : nullptr);
                }
            }
        } catch (...) {
            restore_logging();
            throw;
        }

        restore_logging();
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
