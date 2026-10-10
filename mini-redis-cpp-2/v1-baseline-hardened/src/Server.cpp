#include "Server.h"

#include <csignal>
#include <cstring>
#include <iostream>

#include "Commands.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace redis {

namespace {

std::atomic<bool> g_stop_requested{false};

#ifdef _WIN32
/// Windows 控制台事件（Ctrl+C / Ctrl+Break）。
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
/// 自管道：handler 里只做 write()，把唤醒工作留给主循环（异步信号安全）。
int g_wake_pipe[2] = {-1, -1};

extern "C" void posixSignalHandler(int /*sig*/) {
    g_stop_requested.store(true);
    const char byte = 'x';
    if (g_wake_pipe[1] >= 0) {
        const ssize_t ignored = ::write(g_wake_pipe[1], &byte, 1);
        (void)ignored;
    }
}
#endif

}  // namespace

void installSignalHandlers() {
#ifdef _WIN32
    ::SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    if (::pipe(g_wake_pipe) == 0) {
        // 非阻塞写端：即使管道满也不会让 handler 阻塞
        for (int i = 0; i < 2; ++i) {
            const int flags = ::fcntl(g_wake_pipe[i], F_GETFL, 0);
            if (flags >= 0) ::fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    struct sigaction action {};
    action.sa_handler = posixSignalHandler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // 不加 SA_RESTART：让 accept 能被信号打断
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
#endif
}

void requestShutdown() { g_stop_requested.store(true); }

Server::Server(int port, std::string dump_path) : port_(port), dump_path_(std::move(dump_path)) {}

Server::~Server() { stop(); }

bool Server::saveDatabase() {
    if (database().save(dump_path_)) {
        std::cout << "[server] database dumped to " << dump_path_ << std::endl;
        return true;
    }
    std::cerr << "[server] failed to dump database to " << dump_path_ << std::endl;
    return false;
}

void Server::workerMain(Server& server, const std::shared_ptr<net::Socket>& socket) {
    Client client(*socket);
    while (server.running_.load()) {
        std::vector<std::string> args;
        const resp::ParseState state = client.nextRequest(args);
        if (state == resp::ParseState::kError) {
            client.flush();
            break;
        }
        if (state == resp::ParseState::kRequest) {
            const CommandEffect effect = client.handle(args);
            server.total_commands_.fetch_add(1);
            if (!client.flush()) break;  // 对端断开
            if (effect == CommandEffect::kStopServer) {
                requestShutdown();  // 通知主循环：落盘并退出
                break;
            }
            if (effect == CommandEffect::kCloseConn) break;
            continue;  // 继续处理同一次 recv 中的后续命令（流水线）
        }
        // kNeedMore：读取更多数据
        const int n = client.receive();
        if (n <= 0) break;
    }
    socket->reset();
}

bool Server::run() {
    // 先注册信号处理：之后任何时刻按 Ctrl+C 都能进入优雅退出路径。
    installSignalHandlers();
    listener_ = net::Socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener_.valid()) {
        std::cerr << "[server] failed to create socket: " << net::errorText(net::lastError())
                  << std::endl;
        return false;
    }
    if (!listener_.setReuseAddr(true)) {
        std::cerr << "[server] warning: could not set address reuse: "
                  << net::errorText(net::lastError()) << std::endl;
    }
    if (!listener_.bindAny(static_cast<uint16_t>(port_))) {
        std::cerr << "[server] failed to bind port " << port_ << ": "
                  << net::errorText(net::lastError()) << std::endl;
        return false;
    }
    if (!listener_.listen(128)) {
        std::cerr << "[server] failed to listen: " << net::errorText(net::lastError()) << std::endl;
        return false;
    }

    std::cout << "[server] listening on 0.0.0.0:" << port_ << " (pid "
              <<
#ifdef _WIN32
              ::GetCurrentProcessId()
#else
              ::getpid()
#endif
              << ")" << std::endl;

    Server& self = *this;

    while (running_.load() && !g_stop_requested.load()) {
        // 带 50ms 超时地等待新连接：这样收到退出信号后主循环能及时醒来，
        // 不需要在信号处理器里 exit()（那会让 dump 结果不可预期）。
        const int ready = listener_.waitReadable(50);
        if (ready < 0) {
            if (g_stop_requested.load()) break;
            std::cerr << "[server] wait for connection failed: "
                      << net::errorText(net::lastError()) << std::endl;
            break;
        }
        if (ready == 0) continue;  // 超时：回到循环顶部检查退出标志

        net::Socket clientSocket;
        if (!listener_.accept(clientSocket)) {
            if (g_stop_requested.load()) break;
            std::cerr << "[server] accept failed: " << net::errorText(net::lastError()) << std::endl;
            break;
        }
        if (g_stop_requested.load()) break;  // SHUTDOWN 恰好发生在 accept 返回时
        clientSocket.setNoDelay(true);
        total_connections_.fetch_add(1);
        active_connections_.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            // GCC 16 在为 shared_ptr/std::thread 的析构内联时会对 libstdc++
            // 控制块里的 mutex 报 -Warray-bounds 误报（编译器已知问题，
            // 与本文件的逻辑无关）。这里只关闭这一条诊断，范围限定在
            // “构造 Worker + 登记线程”这几行。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
            Worker worker;
            worker.socket = std::shared_ptr<net::Socket>(new net::Socket(std::move(clientSocket)));
            const auto socket = worker.socket;
            worker.thread = std::thread([&self, socket]() {
                workerMain(self, socket);
                self.active_connections_.fetch_sub(1);
            });
            workers_.emplace(worker.thread.get_id(), std::move(worker));
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }
    }

    stop();

    std::cout << "[server] connections: total=" << total_connections_.load()
              << " commands=" << total_commands_.load() << std::endl;
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) {
        saveDatabase();
        return;
    }
    std::cout << "[server] stopping..." << std::endl;
    listener_.reset();
    std::cout << "[server] listener closed" << std::endl;

    // 主动断开所有连接：worker 的 recv 立即返回，线程得以退出
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& kv : workers_) kv.second.socket->reset();
    }
    std::cout << "[server] " << workers_.size() << " connections closed; joining" << std::endl;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& kv : workers_) {
            if (kv.second.thread.joinable()) kv.second.thread.join();
        }
        workers_.clear();
    }
    std::cout << "[server] threads joined" << std::endl;

    saveDatabase();
    std::cout << "[server] shutdown complete" << std::endl;
}

}  // namespace redis
