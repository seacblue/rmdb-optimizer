/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file stress_oj10_concurrency.cpp
 * @brief 并发压力测试 — 多客户端高并发访问
 *
 * 测试方式：client-server（多线程）
 * 注意：服务器需支持并行连接
 */

#include "../oj/test_oj_client_server.h"

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

class StressConcurrencyTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_concurrency_test_db";
        start_server();
        connect_client();
    }

    void TearDown() override {
        disconnect_client();
        stop_server();
    }

    int create_connection() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(OJ_SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }
        return sock;
    }

    std::string send_on(int sock, const std::string &sql) {
        std::string cmd = sql;
        if (!cmd.empty() && cmd.back() != ';') cmd += ";";
        if (write(sock, cmd.c_str(), cmd.length() + 1) == -1) return "";
        char buf[65536];
        memset(buf, 0, sizeof(buf));
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) return "";
        return std::string(buf);
    }

    void close_connection(int sock) {
        if (sock >= 0) {
            write(sock, "exit", 5);
            close(sock);
        }
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressConcurrencyTest, CONCURRENT_10ClientsHeavyInsert) {
    expect_ok(send_sql("create table t(id int, val int)"));

    const int NUM_CLIENTS = 10;
    const int ROWS_PER_CLIENT = 500;
    std::atomic<int> failures{0};
    std::mutex print_mutex;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int c = 0; c < NUM_CLIENTS; c++) {
        threads.emplace_back([this, c, ROWS_PER_CLIENT, &failures, &print_mutex]() {
            int sock = create_connection();
            if (sock < 0) {
                failures++;
                return;
            }

            // BEGIN
            send_on(sock, "begin");

            for (int i = 1; i <= ROWS_PER_CLIENT; i++) {
                int id = c * ROWS_PER_CLIENT + i;
                std::string r = send_on(sock, "insert into t values(" + std::to_string(id) + ", " + std::to_string(id) + ")");
                if (r.find("Error: ") == 0) {
                    failures++;
                }
            }

            // Commit or abort based on failures
            if (failures == 0) {
                send_on(sock, "commit");
            } else {
                send_on(sock, "abort");
            }

            close_connection(sock);
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int total_rows = NUM_CLIENTS * ROWS_PER_CLIENT;
    std::cout << "\n  [STRESS] CONCURRENT_10ClientsHeavyInsert: " << NUM_CLIENTS
              << " clients x " << ROWS_PER_CLIENT << " rows = " << total_rows
              << " inserts in " << ms << " ms" << std::endl;
    if (failures > 0) {
        std::cout << "    (Note: " << failures.load() << " insert failures due to locking)" << std::endl;
    }
}

TEST_F(StressConcurrencyTest, CONCURRENT_UpdateContention) {
    expect_ok(send_sql("create table account(id int, balance int)"));
    expect_ok(send_sql("insert into account values(1, 10000)"));

    const int NUM_CLIENTS = 8;
    const int OPS_PER_CLIENT = 200;
    std::atomic<int> success_count{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int c = 0; c < NUM_CLIENTS; c++) {
        threads.emplace_back([this, OPS_PER_CLIENT, &success_count]() {
            int sock = create_connection();
            if (sock < 0) return;

            for (int i = 0; i < OPS_PER_CLIENT; i++) {
                std::string r = send_on(sock, "begin");
                r = send_on(sock, "update account set balance = balance + 1 where id = 1");
                if (r.find("Error: ") != 0 && r.find("failure") != 0) {
                    r = send_on(sock, "commit");
                    if (r.find("Error: ") != 0 && r.find("failure") != 0) {
                        success_count++;
                    }
                } else {
                    send_on(sock, "abort");
                }
            }

            close_connection(sock);
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\n  [STRESS] CONCURRENT_UpdateContention: " << NUM_CLIENTS
              << " clients x " << OPS_PER_CLIENT << " attempts on 1 row" << std::endl;
    std::cout << "    Successful updates: " << success_count.load() << " in " << ms << " ms" << std::endl;
}

TEST_F(StressConcurrencyTest, CONCURRENT_ReadOnlyHighLoad) {
    expect_ok(send_sql("create table t(id int, val int)"));
    for (int i = 1; i <= 200; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }

    const int NUM_CLIENTS = 16;
    const int QUERIES_PER_CLIENT = 100;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int c = 0; c < NUM_CLIENTS; c++) {
        threads.emplace_back([this, c, QUERIES_PER_CLIENT]() {
            int sock = create_connection();
            if (sock < 0) return;

            for (int q = 0; q < QUERIES_PER_CLIENT; q++) {
                send_on(sock, "select * from t where id = " + std::to_string((q % 200) + 1));
            }

            close_connection(sock);
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int total_ops = NUM_CLIENTS * QUERIES_PER_CLIENT;
    std::cout << "\n  [STRESS] CONCURRENT_ReadOnlyHighLoad: " << NUM_CLIENTS
              << " clients x " << QUERIES_PER_CLIENT << " queries = " << total_ops
              << " ops in " << ms << " ms (" << (ms * 1000 / total_ops) << " us/op)" << std::endl;
}
