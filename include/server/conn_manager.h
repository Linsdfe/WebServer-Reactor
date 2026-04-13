#ifndef CONN_MANAGER_H
#define CONN_MANAGER_H

#include <list>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <atomic>

namespace reactor {

enum class ConnState {
    ALIVE = 0,
    TIMEOUT = 1,
    CLOSED = 2
};

struct Connection {
    int fd;
    ConnState state;
    std::chrono::steady_clock::time_point last_active;
    // 链表迭代器
    std::list<Connection>::iterator list_iter;

    Connection(int fd_) : fd(fd_), state(ConnState::ALIVE) {
        last_active = std::chrono::steady_clock::now();
    }
};

// 线程安全连接管理器（优化版）
class ConnManager {
public:
    // 关闭连接的回调函数（通知Server销毁连接）
    using CloseCallback = std::function<void(int)>;

    static ConnManager& GetInstance() {
        static ConnManager instance;
        return instance;
    }

    ConnManager(const ConnManager&) = delete;
    ConnManager& operator=(const ConnManager&) = delete;

    // 设置关闭回调
    void SetCloseCallback(CloseCallback cb) {
        std::lock_guard<std::mutex> lock(mtx_);
        close_cb_ = std::move(cb);
    }

    // 添加连接 O(1)
    void AddConn(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (fd_map_.count(fd)) return;

        conn_list_.emplace_front(fd);
        auto iter = conn_list_.begin();
        iter->list_iter = iter;
        fd_map_[fd] = iter;
    }

    // 更新心跳 O(1)
    bool UpdateHeartbeat(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = fd_map_.find(fd);
        if (it == fd_map_.end()) return false;

        auto& conn = *it->second;
        if (conn.state != ConnState::ALIVE) return false;
        conn.last_active = std::chrono::steady_clock::now();
        return true;
    }

    // 超时检测（Reactor定时器调用）
    void CheckTimeout(int timeout_sec) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!close_cb_) return;

        auto now = std::chrono::steady_clock::now();
        auto iter = conn_list_.begin();

        while (iter != conn_list_.end()) {
            auto& conn = *iter;
            if (conn.state == ConnState::CLOSED) {
                fd_map_.erase(conn.fd);
                iter = conn_list_.erase(iter);
                continue;
            }

            // 超时判断
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - conn.last_active);
            if (idle.count() >= timeout_sec) {
                int fd = conn.fd;
                conn.state = ConnState::TIMEOUT;
                fd_map_.erase(fd);
                iter = conn_list_.erase(iter);

                // 不直接close！通知Server安全关闭
                close_cb_(fd);
                std::cout << "[Timeout] FD=" << fd << " idle=" << idle.count() << "s\n";
            } else {
                ++iter;
            }
        }
    }

    // 主动关闭连接 O(1)
    void CloseConn(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = fd_map_.find(fd);
        if (it == fd_map_.end()) return;

        auto& conn = *it->second;
        conn.state = ConnState::CLOSED;
        fd_map_.erase(fd);
        conn_list_.erase(conn.list_iter);
    }

private:
    ConnManager() = default;

    ~ConnManager() {
        std::lock_guard<std::mutex> lock(mtx_);
        conn_list_.clear();
        fd_map_.clear();
    }

    std::list<Connection> conn_list_;          // 连接链表
    std::unordered_map<int, std::list<Connection>::iterator> fd_map_; // fd→迭代器 O(1)查找
    std::mutex mtx_;
    CloseCallback close_cb_; // 关闭连接回调
};

} // namespace reactor
#endif // CONN_MANAGER_H