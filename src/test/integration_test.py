#!/usr/bin/env python3
"""
RMDB Lab2 集成测试客户端

通过 TCP Socket 连接 RMDB 服务端，发送 SQL 语句，并验证 output.txt 的内容。
完全非交互式，不会卡在 readline 等待中。

用法: python3 integration_test.py [--host 127.0.0.1] [--port 8765] [--output PATH]
"""

import socket
import time
import sys
import os
import argparse

HOST = "127.0.0.1"
PORT = 8765
BUF_SIZE = 65536


def send_sql(sock, sql):
    """发送一条 SQL 语句并读取响应"""
    # RMDB parser requires SQL statements to end with semicolon
    sql = sql.strip()
    if not sql.endswith(";"):
        sql = sql + ";"
    sock.sendall(sql.encode("utf-8") + b"\0")
    data = b""
    try:
        while True:
            chunk = sock.recv(BUF_SIZE)
            if not chunk:
                break
            data += chunk
            if b"\0" in chunk:
                break
    except socket.timeout:
        pass
    return data


def read_output_segment(output_path, offset):
    """从 offset 开始读取 output.txt 的新增内容"""
    with open(output_path, "r") as f:
        f.seek(offset)
        content = f.read()
        new_offset = f.tell()
    return content, new_offset


def normalize(text):
    """标准化文本以便比较：去掉行尾空白，去掉空行"""
    lines = [line.rstrip() for line in text.splitlines()]
    return [line for line in lines if line]


def check_output(actual, expected_lines, label):
    """比较实际输出与期望输出"""
    actual_norm = normalize(actual)
    ok = actual_norm == expected_lines
    if ok:
        print(f"  [PASS] {label}")
    else:
        print(f"  [FAIL] {label}")
        print(f"    期望 ({len(expected_lines)} 行):")
        for l in expected_lines:
            print(f"      {repr(l)}")
        print(f"    实际 ({len(actual_norm)} 行):")
        for l in actual_norm:
            print(f"      {repr(l)}")
    return ok


def run_tests(host, port, output_path):
    # 连接到服务端（带重试）
    sock = None
    for attempt in range(10):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((host, port))
            break
        except (ConnectionRefusedError, socket.timeout) as e:
            if sock:
                sock.close()
            if attempt == 9:
                print(f"无法连接到服务端 {host}:{port}: {e}")
                return False
            time.sleep(1)
    print(f"已连接到 RMDB 服务端 {host}:{port}")

    time.sleep(0.5)

    total_pass = 0
    total_fail = 0

    # ═══════════════════════════════════════════
    # 测试点1: 尝试建表 (DDL)
    # ═══════════════════════════════════════════
    print("\n=== 测试点1: 尝试建表 (DDL) ===")
    offset = os.path.getsize(output_path) if os.path.exists(output_path) else 0

    sqls = [
        "create table t1(id int,name char(4))",
        "show tables",
        "create table t2(id int)",
        "show tables",
        "drop table t1",
        "show tables",
        "drop table t2",
        "show tables",
    ]
    for sql in sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected = [
        "| Tables |",
        "| t1 |",
        "| Tables |",
        "| t1 |",
        "| t2 |",
        "| Tables |",
        "| t2 |",
        "| Tables |",
    ]
    if check_output(content, expected, "DDL 建表/删表/show"):
        total_pass += 1
    else:
        total_fail += 1

    # ═══════════════════════════════════════════
    # 测试点2: 单表插入与条件查询
    # ═══════════════════════════════════════════
    print("\n=== 测试点2: 单表插入与条件查询 ===")
    offset = os.path.getsize(output_path)

    sqls = [
        "create table grade (name char(4),id int,score float)",
        "insert into grade values ('Data', 1, 90.5)",
        "insert into grade values ('Data', 2, 95.0)",
        "insert into grade values ('Calc', 2, 92.0)",
        "insert into grade values ('Calc', 1, 88.5)",
        "select * from grade",
        "select score,name,id from grade where score > 90",
        "select id from grade where name = 'Data'",
        "select name from grade where id = 2 and score > 90",
    ]
    for sql in sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected = [
        "| name | id | score |",
        "| Data | 1 | 90.500000 |",
        "| Data | 2 | 95.000000 |",
        "| Calc | 2 | 92.000000 |",
        "| Calc | 1 | 88.500000 |",
        "| score | name | id |",
        "| 90.500000 | Data | 1 |",
        "| 95.000000 | Data | 2 |",
        "| 92.000000 | Calc | 2 |",
        "| id |",
        "| 1 |",
        "| 2 |",
        "| name |",
        "| Data |",
        "| Calc |",
    ]
    if check_output(content, expected, "单表插入与条件查询"):
        total_pass += 1
    else:
        total_fail += 1

    send_sql(sock, "drop table grade")
    time.sleep(0.1)

    # ═══════════════════════════════════════════
    # 测试点3: 单表更新与条件查询
    # ═══════════════════════════════════════════
    print("\n=== 测试点3: 单表更新与条件查询 ===")
    offset = os.path.getsize(output_path)

    sqls = [
        "create table grade (name char(4),id int,score float)",
        "insert into grade values ('Data', 1, 90.5)",
        "insert into grade values ('Data', 2, 95.0)",
        "insert into grade values ('Calc', 2, 92.0)",
        "insert into grade values ('Calc', 1, 88.5)",
        "select * from grade",
        "update grade set score = 99.0 where name = 'Calc'",
        "select * from grade",
        "update grade set name = 'test' where name > 'A'",
        "select * from grade",
        "update grade set name = 'test' ,id = -1,score = 0 where name = 'test' and score > 90",
        "select * from grade",
    ]
    for sql in sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected = [
        "| name | id | score |",
        "| Data | 1 | 90.500000 |",
        "| Data | 2 | 95.000000 |",
        "| Calc | 2 | 92.000000 |",
        "| Calc | 1 | 88.500000 |",
        "| name | id | score |",
        "| Data | 1 | 90.500000 |",
        "| Data | 2 | 95.000000 |",
        "| Calc | 2 | 99.000000 |",
        "| Calc | 1 | 99.000000 |",
        "| name | id | score |",
        "| test | 1 | 90.500000 |",
        "| test | 2 | 95.000000 |",
        "| test | 2 | 99.000000 |",
        "| test | 1 | 99.000000 |",
        "| name | id | score |",
        "| test | -1 | 0.000000 |",
        "| test | -1 | 0.000000 |",
        "| test | -1 | 0.000000 |",
        "| test | -1 | 0.000000 |",
    ]
    if check_output(content, expected, "单表更新与条件查询"):
        total_pass += 1
    else:
        total_fail += 1

    send_sql(sock, "drop table grade")
    time.sleep(0.1)

    # ═══════════════════════════════════════════
    # 测试点4: 单表删除与条件查询
    # ═══════════════════════════════════════════
    print("\n=== 测试点4: 单表删除与条件查询 ===")
    offset = os.path.getsize(output_path)

    sqls = [
        "create table grade (name char(4),id int,score float)",
        "insert into grade values ('Data', 1, 90.5)",
        "select * from grade",
        "delete from grade where score > 90",
        "select * from grade",
    ]
    for sql in sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected = [
        "| name | id | score |",
        "| Data | 1 | 90.500000 |",
        "| name | id | score |",
    ]
    if check_output(content, expected, "单表删除与条件查询"):
        total_pass += 1
    else:
        total_fail += 1

    send_sql(sock, "drop table grade")
    time.sleep(0.1)

    # ═══════════════════════════════════════════
    # 测试点5: 连接查询
    # ═══════════════════════════════════════════
    print("\n=== 测试点5: 连接查询 ===")
    offset = os.path.getsize(output_path)

    sqls = [
        "create table t ( id int , t_name char (3))",
        "create table d (d_name char(5),id int)",
        "insert into t values (1,'aaa')",
        "insert into t values (2,'baa')",
        "insert into t values (3,'bba')",
        "insert into d values ('12345',1)",
        "insert into d values ('23456',2)",
        "select * from t, d",
        "select t.id,t_name,d_name from t,d where t.id = d.id",
    ]
    for sql in sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected = [
        "| id | t_name | d_name | id |",
        "| 1 | aaa | 12345 | 1 |",
        "| 1 | aaa | 23456 | 2 |",
        "| 2 | baa | 12345 | 1 |",
        "| 2 | baa | 23456 | 2 |",
        "| 3 | bba | 12345 | 1 |",
        "| 3 | bba | 23456 | 2 |",
        "| id | t_name | d_name |",
        "| 1 | aaa | 12345 |",
        "| 2 | baa | 23456 |",
    ]
    if check_output(content, expected, "连接查询"):
        total_pass += 1
    else:
        total_fail += 1

    send_sql(sock, "drop table t")
    send_sql(sock, "drop table d")
    time.sleep(0.1)

    # ═══════════════════════════════════════════
    # 额外测试: 错误处理 (非法 SQL → failure)
    # ═══════════════════════════════════════════
    print("\n=== 额外测试: 错误处理 ===")
    send_sql(sock, "create table err_test(id int, name char(4))")
    time.sleep(0.1)

    offset = os.path.getsize(output_path)

    error_sqls = [
        "create table err_test(id int)",          # 表已存在
        "drop table nonexistent_table",            # 表不存在
        "insert into err_test values (1)",         # 值数量不匹配
        "select * from nonexistent_table",         # 查询不存在的表
        "select bad_col from err_test",            # 查询不存在的列
        "delete from err_test where bad_col > 0",  # where 条件中使用不存在的列
    ]
    for sql in error_sqls:
        send_sql(sock, sql)
        time.sleep(0.1)

    content, offset = read_output_segment(output_path, offset)
    expected_failure_count = len(error_sqls)
    failure_count = content.count("failure")
    if failure_count == expected_failure_count:
        print(f"  [PASS] 错误处理 ({failure_count}/{expected_failure_count} 条错误 SQL 正确输出 failure)")
        total_pass += 1
    else:
        print(f"  [FAIL] 错误处理 (期望 {expected_failure_count} 个 failure，实际 {failure_count} 个)")
        print(f"    output.txt 新增内容:")
        for l in content.splitlines():
            print(f"      {repr(l)}")
        total_fail += 1

    send_sql(sock, "drop table err_test")
    time.sleep(0.1)

    sock.close()

    # ═══════════════════════════════════════════
    # 汇总
    # ═══════════════════════════════════════════
    print(f"\n{'='*50}")
    print(f"集成测试结果: {total_pass} 通过, {total_fail} 失败")
    print(f"{'='*50}")

    return total_fail == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="RMDB Lab2 集成测试")
    parser.add_argument("--host", default="127.0.0.1", help="服务端地址")
    parser.add_argument("--port", type=int, default=8765, help="服务端端口")
    parser.add_argument("--output", default=None, help="output.txt 路径")
    args = parser.parse_args()

    output_path = args.output
    if output_path is None:
        output_path = os.path.join("execution_test_db", "output.txt")

    success = run_tests(args.host, args.port, output_path)
    sys.exit(0 if success else 1)