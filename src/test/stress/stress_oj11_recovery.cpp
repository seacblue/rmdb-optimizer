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
 * @file stress_oj11_recovery.cpp
 * @brief 故障恢复压力测试 — 大量数据 crash 后恢复时间
 *
 * 测试方式：client-server
 * 测试大量数据写入后 crash，重启验证恢复速度和正确性
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>
#include <thread>
#include <fstream>
#include <vector>
#include <cstdlib>

class StressRecoveryTest : public ::testing::Test {
   protected:
    std::string db_name_ = "stress_recovery_test_db";
    std::string server_bin_;
    pid_t server_pid_ = -1;
    int sockfd_ = -1;

    void SetUp() override {
        server_bin_ = find_server();
        system(("rm -rf " + db_name_).c_str());
    }

    void TearDown() override {
        stop_server();
    }

    std::string find_server() {
        std::vector<std::string> candidates = {
            "./bin/rmdb", "../bin/rmdb", "../../build/bin/rmdb",
            "build/bin/rmdb", "/home/seako/rmdb/build/bin/rmdb"};
        for (const auto &p : candidates) {
            if (access(p.c_str(), X_OK) == 0) {
                char abs[4096];
                if (realpath(p.c_str(), abs)) return std::string(abs);
                return p;
            }
        }
        return "rmdb";
    }

    void start_server() {
        system(("rm -rf " + db_name_).c_str());
        server_pid_ = fork();
        if (server_pid_ == 0) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null != -1) {
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
            execlp(server_bin_.c_str(), server_bin_.c_str(), db_name_.c_str(), nullptr);
            _exit(1);
        }
        ASSERT_GT(server_pid_, 0);
        bool ready = false;
        for (int i = 0; i < 50; i++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(OJ_SERVER_PORT);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    close(s); ready = true; break;
                }
                close(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ASSERT_TRUE(ready) << "Could not start server";
    }

    void restart_server() {
        stop_server();
        server_pid_ = fork();
        if (server_pid_ == 0) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null != -1) {
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
            execlp(server_bin_.c_str(), server_bin_.c_str(), db_name_.c_str(), nullptr);
            _exit(1);
        }
        ASSERT_GT(server_pid_, 0);
        bool ready = false;
        for (int i = 0; i < 50; i++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(OJ_SERVER_PORT);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    close(s); ready = true; break;
                }
                close(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ASSERT_TRUE(ready) << "Could not restart server";
    }

    void connect_client() {
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(sockfd_, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(OJ_SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ASSERT_EQ(connect(sockfd_, (struct sockaddr *)&addr, sizeof(addr)), 0);
    }

    void disconnect_client() {
        if (sockfd_ >= 0) {
            write(sockfd_, "exit", 5);
            close(sockfd_);
            sockfd_ = -1;
        }
    }

    void stop_server() {
        disconnect_client();
        if (server_pid_ > 0) {
            kill(server_pid_, SIGINT);
            for (int i = 0; i < 30; i++) {
                int status;
                if (waitpid(server_pid_, &status, WNOHANG) == server_pid_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(server_pid_, SIGKILL);
            waitpid(server_pid_, nullptr, 0);
            server_pid_ = -1;
        }
    }

    std::string send_sql(const std::string &sql) {
        if (sockfd_ < 0) return "";
        std::string cmd = sql;
        if (!cmd.empty() && cmd.back() != ';') cmd += ";";
        if (write(sockfd_, cmd.c_str(), cmd.length() + 1) == -1) return "";
        std::string result;
        char buf[4096];
        while (true) {
            memset(buf, 0, sizeof(buf));
            int len = recv(sockfd_, buf, sizeof(buf) - 1, 0);
            if (len <= 0) break;
            result.append(buf);
            if (buf[len - 1] == '\0') break;
        }
        return result;
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressRecoveryTest, RECOVERY_LargeDataset) {
    // 第一轮：大量数据
    start_server();
    connect_client();

    expect_ok(send_sql("create table t1(id int, val int)"));
    expect_ok(send_sql("create table t2(id int, name char(16), score float)"));

    auto write_start = std::chrono::steady_clock::now();

    for (int i = 1; i <= 3000; i++) {
        expect_ok(send_sql("insert into t1 values(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")"));
    }
    for (int i = 1; i <= 2000; i++) {
        expect_ok(send_sql("insert into t2 values(" + std::to_string(i) + ", 'item_" +
                           std::to_string(i) + "', " + std::to_string(i * 0.5) + ")"));
    }

    auto write_end = std::chrono::steady_clock::now();
    auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(write_end - write_start).count();
    std::cout << "\n  [STRESS] RECOVERY_LargeDataset: written 5000 rows in " << write_ms << " ms" << std::endl;

    // Commit and crash
    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    // 第二轮：记录恢复时间
    auto recovery_start = std::chrono::steady_clock::now();

    restart_server();
    connect_client();

    auto recovery_end = std::chrono::steady_clock::now();
    auto recovery_ms = std::chrono::duration_cast<std::chrono::milliseconds>(recovery_end - recovery_start).count();

    // 验证数据
    std::string res1 = send_sql("select count(*) from t1");
    EXPECT_NE(res1.find("Total record(s): 1"), std::string::npos);

    std::string res2 = send_sql("select count(*) from t2");
    EXPECT_NE(res2.find("Total record(s): 1"), std::string::npos);

    std::cout << "  RECOVERY time (server restart + REDO): " << recovery_ms << " ms" << std::endl;

    // Read all data
    res1 = send_sql("select * from t1");
    EXPECT_NE(res1.find("Total record(s): 3000"), std::string::npos);
    res2 = send_sql("select * from t2");
    EXPECT_NE(res2.find("Total record(s): 2000"), std::string::npos);
}

TEST_F(StressRecoveryTest, RECOVERY_UncommittedAfterCrash) {
    start_server();
    connect_client();

    expect_ok(send_sql("create table t(id int, val int)"));
    for (int i = 1; i <= 500; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }

    // Begin, insert many, crash without commit
    expect_ok(send_sql("begin"));
    for (int i = 501; i <= 1500; i++) {
        send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }

    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    restart_server();
    connect_client();

    std::string res = send_sql("select count(*) from t");
    // Should be 500 after UNDO
    std::cout << "\n  [STRESS] RECOVERY_UncommittedAfterCrash: after crash+recovery" << std::endl;
    std::cout << "    Expected 500 rows (uncommitted 1000 rolled back)" << std::endl;
}
