#include "Server.h"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>

#include "Commands.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace redis {

namespace {

std::atomic<bool> g_stop_requested{false};

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
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

/// 默认线程数：CPU 核数（下限 2，上限 64）。
size_t defaultThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t count = hw == 0 ? 4 : static_cast<size_t>(hw);
    return std::min<size_t>(64, std::max<size_t>(2, count));
}

}  // namespace

size_t defaultThreadCount() { return defaultThreads(); }

void installSignalHandlers() {
#ifdef _WIN32
    ::SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    if (::pipe(g_wake_pipe) == 0) {
        for (int i = 0; i < 2; ++i) {
            const int flags = ::fcntl(g_wake_pipe[i], F_GETFL, 0);
            if (flags >= 0) ::fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    struct sigaction action {};
    action.sa_handler = posixSignalHandler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // 不加 SA_RESTART：让系统调用能被信号打断
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
#endif
}

void requestShutdown() { g_stop_requested.store(true); }

Server::Server(Options options) : options_(std::move(options)) {
    if (options_.threads == 0) options_.threads = defaultThreads();
    RuntimeConfig config;
    config.maxmemory = database().maxmemory();
    config.idle_timeout_ms = options_.idle_timeout_ms;
    config.max_clients = options_.max_clients;
    config.dump_interval_ms = options_.dump_interval_ms;
    database().updateConfig(config);
}

Server::~Server() { stop(); }

RuntimeConfig Server::liveConfig() const { return database().config(); }

bool Server::saveDatabase() {
    const SaveResult result = database().save(options_.dump_path);
    if (result == SaveResult::Ok) {
        std::cout << "[server] snapshot written to " << options_.dump_path << std::endl;
        return true;
    }
    if (result == SaveResult::Busy) {
        std::cout << "[server] snapshot skipped: another snapshot is in progress" << std::endl;
        return false;
    }
    std::cerr << "[server] failed to write snapshot to " << options_.dump_path << std::endl;
    return false;
}

void Server::connectionWorker(net::Socket& socket) {
    Executor executor(database());
    Client client(socket, executor);
    const int poll_interval_ms = 100;  // 退出响应粒度
    auto last_read_at = std::chrono::steady_clock::now();

    while (running_.load() && !g_stop_requested.load()) {
        std::vector<std::string> args;
        const resp::ParseState state = client.nextRequest(args);
        if (state == resp::ParseState::kError) {
            client.flush();
            break;
        }
        if (state == resp::ParseState::kRequest) {
            const CommandEffect effect = client.handle(args);
            if (effect == CommandEffect::kStopServer) {
                client.flush();
                requestShutdown();
                break;
            }
            if (effect == CommandEffect::kCloseConn) {
                client.flush();
                break;
            }
            // 批量回写：缓冲未达阈值就继续攒下一条命令的响应
            if (!client.outputFull()) continue;
            if (!client.flush()) break;
            continue;
        }

        // 需要更多数据：先把已攒的响应发出去，避免客户端在等响应而我们在等输入
        if (client.hasPendingOutput() && !client.flush()) break;

        // 短暂的带超时等待：既能及时响应退出信号，也能实现空闲超时
        const int wait_ms = poll_interval_ms;
        const int ready = socket.waitReadable(wait_ms);
        if (ready < 0) break;
        if (ready == 0) {
            const RuntimeConfig config = liveConfig();
            if (config.idle_timeout_ms > 0) {
                const auto idle_since_read = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - last_read_at)
                                                 .count();
                if (idle_since_read > config.idle_timeout_ms) {
                    std::cout << "[server] closing idle connection after " << idle_since_read
                              << " ms" << std::endl;
                    break;
                }
            }
            continue;
        }
        const int n = client.receive();
        if (n <= 0) break;
        last_read_at = std::chrono::steady_clock::now();
    }
    client.flush();
    socket.reset();
}

void Server::backgroundSnapshotLoop() {
    for (;;) {
        RuntimeConfig config = liveConfig();
        int interval = config.dump_interval_ms;
        if (interval <= 0) interval = 300000;

        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        snapshot_wakeup_.wait_for(lock, std::chrono::milliseconds(interval),
                                  [this] { return snapshot_stop_.load(); });
        const bool stopping = snapshot_stop_.load();
        lock.unlock();
        if (stopping) break;

        if (saveDatabase()) {
            std::cout << "[server] periodic snapshot done" << std::endl;
        }
    }
}

bool Server::run() {
    installSignalHandlers();

    listener_ = net::Socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener_.valid()) {
        std::cerr << "[server] failed to create socket: " << net::errorText(net::lastError())
                  << std::endl;
        return false;
    }
    listener_.setReuseAddr(true);
    if (!listener_.bindAny(static_cast<uint16_t>(options_.port))) {
        std::cerr << "[server] failed to bind port " << options_.port << ": "
                  << net::errorText(net::lastError()) << std::endl;
        return false;
    }
    if (!listener_.listen(512)) {
        std::cerr << "[server] failed to listen: " << net::errorText(net::lastError()) << std::endl;
        return false;
    }

    const RuntimeConfig config = liveConfig();
    pool_.setHandler([this](net::Socket& socket) {
        connectionWorker(socket);
        active_connections_.fetch_sub(1);  // 连接真正处理完毕，回收计数
    });
    pool_.start(options_.threads, static_cast<size_t>(std::max(1, config.max_clients)));

    snapshot_thread_ = std::thread([this] { backgroundSnapshotLoop(); });

    std::cout << "[server] " << kServerVersion << " listening on 0.0.0.0:" << options_.port
              << " (pid "
#ifdef _WIN32
              << ::GetCurrentProcessId()
#else
              << ::getpid()
#endif
              << ", threads " << pool_.threadCount() << ", maxclients " << config.max_clients << ")"
              << std::endl;

    while (running_.load() && !g_stop_requested.load()) {
        // 带超时地等待新连接：使退出信号能在 50ms 内被响应
        const int ready = listener_.waitReadable(50);
        if (ready < 0) {
            if (g_stop_requested.load()) break;
            std::cerr << "[server] wait for connection failed: "
                      << net::errorText(net::lastError()) << std::endl;
            break;
        }
        if (ready == 0) continue;

        net::Socket clientSocket;
        if (!listener_.accept(clientSocket)) {
            if (g_stop_requested.load()) break;
            std::cerr << "[server] accept failed: " << net::errorText(net::lastError()) << std::endl;
            break;
        }

        const RuntimeConfig live = liveConfig();
        const int active = active_connections_.load();
        if (live.max_clients > 0 && active >= live.max_clients) {
            // 明确拒绝：给客户端一个可读的原因再关闭
            net::Socket rejected(std::move(clientSocket));
            database().noteRejectedConnection();
            rejected.sendAll("-ERR max number of clients reached\r\n");
            continue;
        }

        database().noteConnection();
        active_connections_.fetch_add(1);
        net::Socket accepted(std::move(clientSocket));
        accepted.setNoDelay(true);
        if (!pool_.submit(std::move(accepted))) {
            // 队列满：明确拒绝并回收计数
            net::Socket rejected(std::move(clientSocket));
            database().noteRejectedConnection();
            rejected.sendAll("-ERR max number of clients reached\r\n");
            active_connections_.fetch_sub(1);
        }
    }

    stop();
    std::cout << "[server] shutdown complete" << std::endl;
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    std::cout << "[server] stopping..." << std::endl;

    listener_.reset();

    snapshot_stop_.store(true);
    snapshot_wakeup_.notify_all();
    if (snapshot_thread_.joinable()) snapshot_thread_.join();

    pool_.stop(5000);
    saveDatabase();
}

}  // namespace redis
