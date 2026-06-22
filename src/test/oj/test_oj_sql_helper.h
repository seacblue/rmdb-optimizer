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
 * @file test_oj_sql_helper.h
 * @brief SQL 执行辅助函数 — 通过 RMDB 完整流水线执行 SQL 语句
 *
 * 用法:
 *   1. 在测试夹具中添加成员: SqlHelper sql_helper_;
 *   2. 在 SetUp() 中初始化: sql_helper_.init(sm_manager_, ...);
 *   3. 调用 sql_helper_.execute_sql("CREATE TABLE ...") 执行 SQL
 *   4. 返回值是 data_send 缓冲区的内容（SELECT 的输出或错误信息）
 */

#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <vector>

#include "analyze/analyze.h"
#include "common/common.h"
#include "common/config.h"
#include "common/context.h"
#include "execution/execution_manager.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parser.h"
#include "parser/parser_defs.h"
#include "portal.h"
#include "recovery/log_manager.h"
#include "system/sm.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"

/**
 * @brief SQL 执行辅助类
 *
 * 封装完整的 SQL → 分析 → 优化 → 执行流水线，
 * 输出结果写入 data_send 缓冲区并作为字符串返回。
 * 异常（RMDBError / TransactionAbortException）被捕获并写入输出，
 * 测试可通过检查返回字符串判断执行结果。
 */
class SqlHelper {
   public:
    SqlHelper()
        : sm_manager_(nullptr),
          lock_manager_(nullptr),
          txn_manager_(nullptr),
          ql_manager_(nullptr),
          log_manager_(nullptr),
          planner_(nullptr),
          optimizer_(nullptr),
          portal_(nullptr),
          analyze_(nullptr) {
        pthread_mutex_init(&buffer_mutex_, nullptr);
    }

    ~SqlHelper() { pthread_mutex_destroy(&buffer_mutex_); }

    /**
     * @brief 初始化 — 传入各管理器指针
     */
    void init(SmManager *sm, LockManager *lm, TransactionManager *tm, QlManager *qm,
              LogManager *logm, Planner *p, Optimizer *opt, Portal *port, Analyze *an) {
        sm_manager_ = sm;
        lock_manager_ = lm;
        txn_manager_ = tm;
        ql_manager_ = qm;
        log_manager_ = logm;
        planner_ = p;
        optimizer_ = opt;
        portal_ = port;
        analyze_ = an;
        txn_id_ = INVALID_TXN_ID;
    }

    /**
     * @brief 执行单条 SQL 语句
     * @param sql SQL 语句字符串
     * @return data_send 缓冲区内容（SELECT 的输出；成功无输出时返回空字符串；
     *         异常时返回错误消息，末尾以 '\n' 结尾）
     */
    std::string execute_sql(const std::string &sql) {
        char data_send[BUFFER_LENGTH];
        memset(data_send, 0, BUFFER_LENGTH);
        int offset = 0;

        Context *context = new Context(lock_manager_, log_manager_, nullptr, data_send, &offset);
        set_transaction(context);

        bool finish_analyze = false;
        pthread_mutex_lock(&buffer_mutex_);

        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        if (yyparse() == 0) {
            if (ast::parse_tree != nullptr) {
                try {
                    std::shared_ptr<Query> query = analyze_->do_analyze(ast::parse_tree);
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(&buffer_mutex_);

                    std::shared_ptr<Plan> plan = optimizer_->plan_query(query, context);
                    std::shared_ptr<PortalStmt> portalStmt = portal_->start(plan, context);
                    portal_->run(portalStmt, ql_manager_, &txn_id_, context);
                    portal_->drop();
                } catch (TransactionAbortException &e) {
                    std::string str = "abort\n";
                    memcpy(data_send, str.c_str(), str.length());
                    data_send[str.length()] = '\0';
                    offset = str.length();
                    txn_manager_->abort(context->txn_, log_manager_);
                } catch (RMDBError &e) {
                    memcpy(data_send, e.what(), e.get_msg_len());
                    data_send[e.get_msg_len()] = '\n';
                    data_send[e.get_msg_len() + 1] = '\0';
                    offset = e.get_msg_len() + 1;
                }
            }
        }
        if (!finish_analyze) {
            yy_delete_buffer(buf);
            pthread_mutex_unlock(&buffer_mutex_);
        }

        // 自动提交非显式事务
        if (context->txn_->get_txn_mode() == false) {
            txn_manager_->commit(context->txn_, context->log_mgr_);
            txn_id_ = INVALID_TXN_ID;
        }

        std::string result(data_send);
        delete context;
        return result;
    }

   private:
    void set_transaction(Context *context) {
        if (txn_id_ == INVALID_TXN_ID) {
            context->txn_ = txn_manager_->begin(nullptr, context->log_mgr_);
            txn_id_ = context->txn_->get_transaction_id();
            context->txn_->set_txn_mode(false);
            return;
        }
        context->txn_ = txn_manager_->get_transaction(txn_id_);
        if (context->txn_ == nullptr ||
            context->txn_->get_state() == TransactionState::COMMITTED ||
            context->txn_->get_state() == TransactionState::ABORTED) {
            context->txn_ = txn_manager_->begin(nullptr, context->log_mgr_);
            txn_id_ = context->txn_->get_transaction_id();
            context->txn_->set_txn_mode(false);
        }
    }

    SmManager *sm_manager_;
    LockManager *lock_manager_;
    TransactionManager *txn_manager_;
    QlManager *ql_manager_;
    LogManager *log_manager_;
    Planner *planner_;
    Optimizer *optimizer_;
    Portal *portal_;
    Analyze *analyze_;
    pthread_mutex_t buffer_mutex_;
    txn_id_t txn_id_ = INVALID_TXN_ID;
};
