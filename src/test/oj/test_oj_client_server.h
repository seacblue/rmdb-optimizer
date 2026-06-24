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

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OJ_SERVER_PORT 8765
#define OJ_BUF_SIZE 65536

/**
 * @brief Client-Server 测试夹具基类
 *
 * 启动 rmdb server 子进程，通过 TCP socket 发送 SQL 并读取响应。
 * 每个 TEST_F 开始前 SetUp 启动 server，TearDown 关闭 server。
 *
 * 用法：
 *   class MyTest : public ClientServerTest { ... };
 *   TEST_F(MyTest, SomeCase) {
 *       auto res = send_sql("CREATE TABLE t (id INT);");
 *       EXPECT_TRUE(res.empty());
 *   }
 */
class ClientServerTest : public ::testing::Test {
   protected:
    pid_t server_pid_ = -1;
    int sockfd_ = -1;
    std::string db_name_;

    void SetUp() override {
        // 子类可以覆盖 db_name_
        if (db_name_.empty()) {
            db_name_ = "oj_test_db";
        }
        start_server();
        connect_client();
    }

    void TearDown() override {
        disconnect_client();
        stop_server();
    }

    /**
     * @brief 发送 SQL 并读取服务端响应（不含 null 终止符）
     */
    std::string send_sql(const std::string &sql) {
        std::string cmd = sql;
        if (!cmd.empty() && cmd.back() != ';') {
            cmd += ";";
        }

        if (write(sockfd_, cmd.c_str(), cmd.length() + 1) == -1) {
            std::cerr << "send_sql: write error" << std::endl;
            return "";
        }

        // Read in a loop until we get the complete null-terminated response.
        // TCP is a stream protocol - a single recv() may not return all data.
        std::string result;
        char buf[OJ_BUF_SIZE];
        while (true) {
            memset(buf, 0, OJ_BUF_SIZE);
            int len = recv(sockfd_, buf, OJ_BUF_SIZE - 1, 0);
            if (len <= 0) {
                break;
            }
            result.append(buf, len);
            if (buf[len - 1] == '\0') {
                break;
            }
        }
        // 去掉尾部的 '\0'
        if (!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }

    /**
     * @brief 发送一批 SQL（自动带 200ms 间隔，模拟客户端行为）
     */
    std::vector<std::string> send_batch(const std::vector<std::string> &sqls, int interval_ms = 100) {
        std::vector<std::string> results;
        for (const auto &sql : sqls) {
            results.push_back(send_sql(sql));
            if (interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }
        return results;
    }

    // 断言辅助函数
    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || res.find("Error: ") != 0 || res.find("failure") != 0)
            << "Expected success but got: " << res;
    }

    void expect_error(const std::string &res) {
        EXPECT_TRUE(res.find("Error: ") == 0 || res.find("failure") == 0)
            << "Expected error but got: " << res;
    }

    void expect_output_contains(const std::string &res, const std::string &sub) {
        EXPECT_NE(res.find(sub), std::string::npos)
            << "Expected output to contain \"" << sub << "\", got: " << res;
    }

    void expect_output_not_contains(const std::string &res, const std::string &sub) {
        EXPECT_EQ(res.find(sub), std::string::npos)
            << "Expected output to NOT contain \"" << sub << "\", got: " << res;
    }

    void expect_total_records(const std::string &res, int n) {
        std::string expected = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(expected), std::string::npos)
            << "Expected \"" << expected << "\", got: [" << res << "]";
    }

   protected:
    void start_server() {
        std::string bin_path = find_server_binary();
        std::string db_path = db_name_;

        // 先清理旧的数据库目录
        std::string cleanup_cmd = "rm -rf " + db_path;
        system(cleanup_cmd.c_str());

        server_pid_ = fork();
        if (server_pid_ == 0) {
            // 子进程：重定向输出到 /dev/null，启动 server
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null != -1) {
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
            execlp(bin_path.c_str(), bin_path.c_str(), db_path.c_str(), nullptr);
            // execlp 失败
            _exit(1);
        } else if (server_pid_ > 0) {
            // 父进程：等待 server 就绪
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

    void connect_client() {
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(sockfd_, 0) << "Failed to create socket";

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(OJ_SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        int ret = connect(sockfd_, (struct sockaddr *)&addr, sizeof(addr));
        ASSERT_EQ(ret, 0) << "Failed to connect to server (errno=" << errno << ")";
    }

    void disconnect_client() {
        if (sockfd_ >= 0) {
            // 发送 exit 命令
            write(sockfd_, "exit", 5);
            close(sockfd_);
            sockfd_ = -1;
        }
    }

    void stop_server() {
        if (server_pid_ > 0) {
            // 发送 SIGINT 让 server 优雅关闭
            kill(server_pid_, SIGINT);
            // 等待最多 3 秒
            int status;
            for (int i = 0; i < 30; i++) {
                pid_t ret = waitpid(server_pid_, &status, WNOHANG);
                if (ret == server_pid_) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // 如果还没退出，强制 kill
            kill(server_pid_, SIGKILL);
            waitpid(server_pid_, nullptr, 0);
            server_pid_ = -1;
        }
    }

    std::string find_server_binary() {
        // 尝试几个常见路径
        std::vector<std::string> candidates = {
            "./bin/rmdb",
            "../bin/rmdb",
            "../../build/bin/rmdb",
            "build/bin/rmdb",
            "/home/seako/rmdb/build/bin/rmdb"
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
        // 如果找不到，返回默认，execlp 会依赖 PATH
        return "rmdb";
    }
};
