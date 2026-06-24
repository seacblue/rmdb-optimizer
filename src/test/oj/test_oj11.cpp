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
 * @file test_oj11.cpp
 * @brief OJ Problem 11 — 故障恢复（WAL + REDO/UNDO + ARIES）
 *
 * 测试方式：client-server
 * 覆盖：crash 后 recovery 检查数据一致性
 *
 * 注意：本测试依次启动两次 server 进程：
 *   第一次：建表、插入数据、发 crash 信号
 *   第二次：重启后 SELECT 检查数据是否恢复
 */

#include "test_oj_client_server.h"

#include <chrono>
#include <thread>
#include <fstream>

class OJ11Test : public ::testing::Test {
   protected:
    std::string db_name_ = "oj11_crash_recovery_test_db";
    std::string server_bin_;
    pid_t server_pid_ = -1;
    int sockfd_ = -1;

    void SetUp() override {
        server_bin_ = find_server_binary();
        // 清理旧的数据库目录
        system(("rm -rf " + db_name_).c_str());
    }

    void TearDown() override {
        stop_server();
    }

    void start_server() {
        // 先清理旧的数据库目录
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
        } else if (server_pid_ > 0) {
            bool ready = false;
            for (int i = 0; i < 50; i++) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(OJ_SERVER_PORT);
                    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        close(sock);
                        ready = true;
                        break;
                    }
                    close(sock);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ASSERT_TRUE(ready) << "Server failed to start within 5 seconds";
        } else {
            FAIL() << "fork() failed";
        }
    }

    void restart_server() {
        stop_server();
        // 不清理 db 目录 — 用已有数据重启
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
        } else if (server_pid_ > 0) {
            bool ready = false;
            for (int i = 0; i < 50; i++) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(OJ_SERVER_PORT);
                    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        close(sock);
                        ready = true;
                        break;
                    }
                    close(sock);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ASSERT_TRUE(ready) << "Server failed to restart within 5 seconds";
        } else {
            FAIL() << "fork() failed";
        }
    }

    void connect_client() {
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(sockfd_, 0);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(OJ_SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        int ret = connect(sockfd_, (struct sockaddr *)&addr, sizeof(addr));
        ASSERT_EQ(ret, 0);
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
                pid_t ret = waitpid(server_pid_, &status, WNOHANG);
                if (ret == server_pid_) break;
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

        char buf[65536];
        memset(buf, 0, sizeof(buf));
        int len = recv(sockfd_, buf, sizeof(buf) - 1, 0);
        if (len <= 0) return "";
        return std::string(buf);
    }

    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0))
            << "Expected success but got: " << res;
    }

    void expect_total_records(const std::string &res, int n) {
        std::string expected = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(expected), std::string::npos)
            << "Expected \"" << expected << "\", got: [" << res << "]";
    }

    void expect_output_contains(const std::string &res, const std::string &sub) {
        EXPECT_NE(res.find(sub), std::string::npos)
            << "Expected output to contain \"" << sub << "\", got: " << res;
    }

    void expect_output_not_contains(const std::string &res, const std::string &sub) {
        EXPECT_EQ(res.find(sub), std::string::npos)
            << "Expected output to NOT contain \"" << sub << "\", got: " << res;
    }

    std::string find_server_binary() {
        std::vector<std::string> candidates = {
            "./bin/rmdb", "../bin/rmdb", "../../build/bin/rmdb",
            "build/bin/rmdb", "/home/seako/rmdb/build/bin/rmdb"
        };
        for (const auto &path : candidates) {
            if (access(path.c_str(), X_OK) == 0) {
                char abs_path[4096];
                if (realpath(path.c_str(), abs_path) != nullptr) {
                    return std::string(abs_path);
                }
                return path;
            }
        }
        return "rmdb";
    }
};

TEST_F(OJ11Test, CommittedDataSurvivesCrash) {
    // 第一轮：启动 server，插入已提交数据，crash
    start_server();
    connect_client();

    expect_success(send_sql("create table t1 (id int, num int)"));
    expect_success(send_sql("insert into t1 values(1, 1)"));
    expect_success(send_sql("insert into t1 values(2, 2)"));

    // 模拟 crash
    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    // 第二轮：重启 server，检查数据
    restart_server();
    connect_client();

    std::string res = send_sql("select * from t1");
    expect_total_records(res, 2);
    expect_output_contains(res, "1");
    expect_output_contains(res, "2");
}

TEST_F(OJ11Test, UncommittedDataRolledBackAfterCrash) {
    // 第一轮
    start_server();
    connect_client();

    expect_success(send_sql("create table t1 (id int, num int)"));
    expect_success(send_sql("insert into t1 values(1, 1)"));

    // 未提交事务
    send_sql("begin");
    expect_success(send_sql("insert into t1 values(99, 99)"));

    // crash — 不提交
    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    // 第二轮：重启
    restart_server();
    connect_client();

    std::string res = send_sql("select * from t1");
    expect_total_records(res, 1);
    expect_output_contains(res, "1");
    expect_output_not_contains(res, "99");
}

TEST_F(OJ11Test, MultiTableCrashRecovery) {
    start_server();
    connect_client();

    expect_success(send_sql("create table t1 (id int, num int)"));
    expect_success(send_sql("create table t2 (name char(8), val float)"));

    for (int i = 1; i <= 5; i++) {
        expect_success(send_sql("insert into t1 values(" + std::to_string(i) + ", " + std::to_string(i * 100) + ")"));
        expect_success(send_sql("insert into t2 values('item', " + std::to_string(i * 1.5) + ")"));
    }

    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    restart_server();
    connect_client();

    std::string res = send_sql("select * from t1");
    expect_total_records(res, 5);

    res = send_sql("select * from t2");
    expect_total_records(res, 5);
}

TEST_F(OJ11Test, JoinAfterCrashRecovery) {
    start_server();
    connect_client();

    expect_success(send_sql("create table t1 (id int, num int)"));
    expect_success(send_sql("create table t2 (t_id int, descr char(8))"));

    expect_success(send_sql("insert into t1 values(1, 10)"));
    expect_success(send_sql("insert into t1 values(2, 20)"));
    expect_success(send_sql("insert into t2 values(1, 'desc1')"));
    expect_success(send_sql("insert into t2 values(2, 'desc2')"));

    disconnect_client();
    kill(server_pid_, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(server_pid_, SIGKILL);
    waitpid(server_pid_, nullptr, 0);
    server_pid_ = -1;

    restart_server();
    connect_client();

    std::string res = send_sql("select * from t1, t2 where t1.id = t2.t_id");
    expect_total_records(res, 2);
}
