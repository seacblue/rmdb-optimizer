/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "errors.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 8

static bool should_exit = false;

namespace {

std::string trim_copy(const char *input) {
    std::string str = input == nullptr ? "" : std::string(input);
    auto begin = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

}  // namespace

// 构建全局所需的管理器对象
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
auto ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
auto sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
auto lock_manager = std::make_unique<LockManager>();
auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
auto ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get());
auto log_manager = std::make_unique<LogManager>(disk_manager.get());
auto recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get());
auto planner = std::make_unique<Planner>(sm_manager.get());
auto optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
auto portal = std::make_unique<Portal>(sm_manager.get());
auto analyze = std::make_unique<Analyze>(sm_manager.get());
pthread_mutex_t *buffer_mutex;
pthread_mutex_t *sockfd_mutex;

static jmp_buf jmpbuf;
void sigint_handler(int signo) {
    should_exit = true;
    log_manager->flush_log_to_disk();
    std::cout << "The Server receive Crtl+C, will been closed\n";
    longjmp(jmpbuf, 1);
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context) {
    context->txn_ = txn_manager->get_transaction(*txn_id);
    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_);
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
    }
}

void append_output_file(const std::string &text) {
    if (!g_output_file_on.load()) {
        return;
    }
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << text;
    outfile.close();
}

bool process_sql_command(const std::string &command, txn_id_t *txn_id, char *data_send, int *offset, bool *should_close) {
    *should_close = false;
    std::string trimmed_cmd = trim_copy(command.c_str());
    if (trimmed_cmd.empty()) {
        return true;
    }
    if (trimmed_cmd == "exit") {
        *should_close = true;
        return true;
    }
    if (trimmed_cmd == "crash") {
        std::cout << "Server crash" << std::endl;
        exit(1);
    }
    if (trimmed_cmd == "set output_file off") {
        g_output_file_on.store(false);
        return true;
    }
    if (trimmed_cmd == "set output_file on") {
        g_output_file_on.store(true);
        return true;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;

    Context context(lock_manager.get(), log_manager.get(), nullptr, data_send, offset);
    SetTransaction(txn_id, &context);

    bool finish_analyze = false;
    pthread_mutex_lock(buffer_mutex);
    YY_BUFFER_STATE buf = yy_scan_string(command.c_str());
    int parse_ret = yyparse();
    if (parse_ret == 0 && ast::parse_tree != nullptr) {
        try {
            std::shared_ptr<Query> query = analyze->do_analyze(ast::parse_tree);
            yy_delete_buffer(buf);
            finish_analyze = true;
            pthread_mutex_unlock(buffer_mutex);
            std::shared_ptr<Plan> plan = optimizer->plan_query(query, &context);
            std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, &context);
            portal->run(portalStmt, ql_manager.get(), txn_id, &context);
            portal->drop();
        } catch (TransactionAbortException &e) {
            std::string str = "abort\n";
            memcpy(data_send, str.c_str(), str.length());
            data_send[str.length()] = '\0';
            *offset = static_cast<int>(str.length());
            txn_manager->abort(context.txn_, log_manager.get());
            std::cout << e.GetInfo() << std::endl;
            append_output_file(str);
        } catch (RMDBError &e) {
            std::cerr << e.what() << std::endl;
            memcpy(data_send, e.what(), e.get_msg_len());
            data_send[e.get_msg_len()] = '\n';
            data_send[e.get_msg_len() + 1] = '\0';
            *offset = e.get_msg_len() + 1;
            append_output_file("failure\n");
        }
    } else {
        std::string failure = "failure\n";
        memcpy(data_send, failure.c_str(), failure.length());
        data_send[failure.length()] = '\0';
        *offset = static_cast<int>(failure.length());
        append_output_file("failure\n");
    }

    if (finish_analyze == false) {
        yy_delete_buffer(buf);
        pthread_mutex_unlock(buffer_mutex);
    }
    ast::parse_tree.reset();

    if (context.txn_ != nullptr && context.txn_->get_txn_mode() == false &&
        context.txn_->get_state() != TransactionState::COMMITTED &&
        context.txn_->get_state() != TransactionState::ABORTED) {
        txn_manager->commit(context.txn_, context.log_mgr_);
    }
    return true;
}

// 从累积缓冲中切出所有"完整语句"。
// 语句边界只有 ';'（SQL 终止符）。'\0' 仅是网络消息边界，不是语句边界——
// 把它当作普通空白处理，从而让跨行 / 跨包的语句片段能正确拼接。
// 未以 ';' 结束的尾部残片保留在 pending 中，等待后续 read 补齐。
std::vector<std::string> extract_statements(std::string &pending) {
    // 先把所有 '\0' 归一成空格，避免它把一条语句切断
    for (auto &ch : pending) {
        if (ch == '\0') ch = ' ';
    }
    std::vector<std::string> stmts;
    size_t start = 0;
    size_t consumed = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i] == ';') {
            // 含分号一并交给 parser（文法为 stmt ';'）
            stmts.push_back(pending.substr(start, i - start + 1));
            start = i + 1;
            consumed = start;
        }
    }
    pending.erase(0, consumed);
    return stmts;
}

// 不带分号的特殊命令（exit / set output_file / crash）：只有当缓冲中没有分号、
// 且 trim 后精确匹配时才识别。返回 true 表示已作为特殊命令处理（并清空 pending）。
bool try_special_command(std::string &pending, bool *should_close) {
    if (pending.find(';') != std::string::npos) return false;
    std::string cmd = trim_copy(pending.c_str());
    if (cmd == "exit") {
        *should_close = true;
        pending.clear();
        return true;
    }
    if (cmd == "crash") {
        std::cout << "Server crash" << std::endl;
        exit(1);
    }
    if (cmd == "set output_file off") {
        g_output_file_on.store(false);
        pending.clear();
        return true;
    }
    if (cmd == "set output_file on") {
        g_output_file_on.store(true);
        pending.clear();
        return true;
    }
    return false;
}

// 处理缓冲中已就绪的全部语句，把各自的回包内容追加到 reply_accum。
void process_ready_statements(std::string &pending, txn_id_t *txn_id, std::string &reply_accum,
                              bool *should_close) {
    *should_close = false;
    // 归一 '\0' 后再检测不带分号的特殊命令
    for (auto &ch : pending) {
        if (ch == '\0') ch = ' ';
    }
    if (try_special_command(pending, should_close)) {
        return;
    }
    std::vector<std::string> stmts = extract_statements(pending);
    std::vector<char> tmp(BUFFER_LENGTH);
    for (auto &stmt : stmts) {
        int off = 0;
        bool close_now = false;
        process_sql_command(stmt, txn_id, tmp.data(), &off, &close_now);
        if (off > 0) {
            reply_accum.append(tmp.data(), static_cast<size_t>(off));
        }
        if (close_now) {
            *should_close = true;
            return;
        }
    }
}

void run_stdin_mode() {
    std::cout << "Socket unavailable, falling back to stdin mode." << std::endl;
    txn_id_t txn_id = INVALID_TXN_ID;
    std::string pending;
    std::string line;
    while (!should_exit && std::getline(std::cin, line)) {
        pending.append(line);
        pending.push_back('\n');
        std::string reply;
        bool should_close = false;
        process_ready_statements(pending, &txn_id, reply, &should_close);
        if (should_close) {
            break;
        }
    }
}

void *client_handler(void *sock_fd) {
    int fd = *((int *)sock_fd);
    pthread_mutex_unlock(sockfd_mutex);

    int i_recvBytes;
    // 接收客户端发送的请求
    char data_recv[BUFFER_LENGTH];
    // 跨多次 read 累积的字节流（兼容一包多条 / 整脚本一次灌入 / 语句被拆到多个包）
    std::string pending;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;

    std::string output = "establish client connection, sockfd: " + std::to_string(fd) + "\n";
    std::cout << output;

    while (true) {
        std::cout << "Waiting for request..." << std::endl;
        memset(data_recv, 0, BUFFER_LENGTH);

        i_recvBytes = read(fd, data_recv, BUFFER_LENGTH);

        if (i_recvBytes == 0) {
            std::cout << "Maybe the client has closed" << std::endl;
            break;
        }
        if (i_recvBytes == -1) {
            std::cout << "Client read error!" << std::endl;
            break;
        }

        printf("i_recvBytes: %d \n ", i_recvBytes);

        // 按收到的实际字节数追加（包含可能的 '\0' 消息边界，用于切分语句）
        pending.append(data_recv, static_cast<size_t>(i_recvBytes));

        std::string reply;
        bool should_close = false;
        process_ready_statements(pending, &txn_id, reply, &should_close);

        // 回包：把本轮所有语句的输出拼在一起返回（保持原客户端一发一收的行为）
        if (reply.empty()) {
            reply.push_back('\0');
        } else if (reply.back() != '\0') {
            reply.push_back('\0');
        }
        if (write(fd, reply.data(), reply.size()) == -1) {
            break;
        }
        if (should_close) {
            std::cout << "Client exit." << std::endl;
            break;
        }
    }

    // Clear
    std::cout << "Terminating current client_connection..." << std::endl;
    close(fd);           // close a file descriptor.
    pthread_exit(NULL);  // terminate calling thread!
}

void start_server() {
    // init mutex
    buffer_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    sockfd_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(buffer_mutex, nullptr);
    pthread_mutex_init(sockfd_mutex, nullptr);

    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    if (sockfd_server == -1) {
        perror("socket");
        run_stdin_mode();
        return;
    }
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        perror("bind");
        close(sockfd_server);
        run_stdin_mode();
        return;
    }

    fd_temp = listen(sockfd_server, MAX_CONN_LIMIT);
    if (fd_temp == -1) {
        perror("listen");
        close(sockfd_server);
        run_stdin_mode();
        return;
    }

    while (!should_exit) {
        std::cout << "Waiting for new connection..." << std::endl;
        pthread_t thread_id;
        struct sockaddr_in s_addr_client {};
        int client_length = sizeof(s_addr_client);

        if (setjmp(jmpbuf)) {
            std::cout << "Break from Server Listen Loop\n";
            break;
        }

        // Block here. Until server accepts a new connection.
        pthread_mutex_lock(sockfd_mutex);
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            std::cout << "Accept error!" << std::endl;
            continue;  // ignore current socket ,continue while loop.
        }
        
        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        if (pthread_create(&thread_id, nullptr, &client_handler, (void *)(&sockfd)) != 0) {
            std::cout << "Create thread fail!" << std::endl;
            break;  // break while loop
        }

    }

    // Clear
    std::cout << " Try to close all client-connection.\n";
    int ret = shutdown(sockfd_server, SHUT_WR);  // shut down the all or part of a full-duplex connection.
    if(ret == -1) { printf("%s\n", strerror(errno)); }
//    assert(ret != -1);
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    try {
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);
        if (g_output_file_on.load()) {
            std::ofstream truncate_output("output.txt", std::ios::out | std::ios::trunc);
        }

        // recovery database
        recovery->analyze();
        recovery->redo();
        recovery->undo();
        
        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return 0;
}
