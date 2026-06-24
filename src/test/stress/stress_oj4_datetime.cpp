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
 * @file stress_oj4_datetime.cpp
 * @brief DATETIME 压力测试 — 大量时间数据操作
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>
#include <sstream>
#include <iomanip>

class StressDateTimeTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_datetime_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }

    std::string make_datetime(int year, int month, int day, int h, int m, int s) {
        std::ostringstream oss;
        oss << "'" << year << "-" << std::setw(2) << std::setfill('0') << month
            << "-" << std::setw(2) << std::setfill('0') << day << " "
            << std::setw(2) << std::setfill('0') << h << ":"
            << std::setw(2) << std::setfill('0') << m << ":"
            << std::setw(2) << std::setfill('0') << s << "'";
        return oss.str();
    }
};

TEST_F(StressDateTimeTest, InsertAndRangeQuery) {
    expect_ok(send_sql("create table t(id int, ts datetime)"));

    auto start = std::chrono::steady_clock::now();

    // 插入 2000 行不同时间
    for (int day = 1; day <= 2000; day++) {
        std::string dt = make_datetime(2024, 1, (day % 28) + 1, 10, 0, 0);
        expect_ok(send_sql("insert into t values(" + std::to_string(day) + ", " + dt + ")"));
    }

    auto mid = std::chrono::steady_clock::now();

    // 范围查询
    for (int q = 0; q < 50; q++) {
        std::string dt1 = make_datetime(2024, 1, (q * 5 % 28) + 1, 0, 0, 0);
        std::string dt2 = make_datetime(2024, 2, 1, 0, 0, 0);
        send_sql("select * from t where ts >= " + dt1 + " and ts <= " + dt2);
    }

    auto end = std::chrono::steady_clock::now();
    auto insert_ms = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start).count();
    auto query_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid).count();

    std::cout << "\n  [STRESS] InsertAndRangeQuery: 2000 datetime inserts in "
              << insert_ms << " ms, 50 range queries in " << query_ms << " ms" << std::endl;
}

TEST_F(StressDateTimeTest, DateTimeWhereDelete) {
    expect_ok(send_sql("create table t(id int, ts datetime)"));

    for (int day = 1; day <= 1000; day++) {
        std::string dt = make_datetime(2024, 3, (day % 31) + 1, 8, 0, 0);
        expect_ok(send_sql("insert into t values(" + std::to_string(day) + ", " + dt + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    // WHERE delete based on datetime
    std::string dt_cut = make_datetime(2024, 3, 15, 0, 0, 0);
    expect_ok(send_sql("delete from t where ts < " + dt_cut));

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] DateTimeWhereDelete: 1000 rows, delete by datetime in "
              << ms << " ms" << std::endl;
}
