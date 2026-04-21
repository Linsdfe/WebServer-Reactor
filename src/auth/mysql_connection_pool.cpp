/**
 * @file mysql_connection_pool.cpp
 * @brief MySQL 连接池实现
 *
 * 连接池原理详解：
 *
 * 1. 为什么需要连接池？
 *    - MySQL 连接的创建和销毁是昂贵的操作
 *    - 频繁的连接操作会消耗大量系统资源
 *    - 连接池预创建连接，减少连接开销
 *    - 控制并发连接数，防止数据库过载
 *
 * 2. 连接池大小计算：
 *    - 经验公式：连接数 = min(线程数 * 2, CPU核心数 * 4 + 1)
 *    - 线程数 * 2：确保每个线程都有足够的连接可用
 *    - CPU核心数 * 4 + 1：基于CPU计算能力，避免过度竞争
 *    - 取最小值：既保证性能，又不会浪费资源
 *
 *    示例：
 *    - 24线程 + 4核CPU：min(48, 17) = 17个连接
 *    - 24线程 + 8核CPU：min(48, 33) = 33个连接
 *    - 24线程 + 16核CPU：min(48, 65) = 48个连接
 *
 * 3. 连接池的工作流程：
 *    初始化阶段：
 *    - 根据系统资源自动计算连接池大小
 *    - 创建指定数量的数据库连接
 *    - 将连接放入空闲队列
 *
 *    获取连接：
 *    - 尝试从空闲队列获取连接
 *    - 如果没有空闲连接且未达到池大小，创建新连接
 *    - 如果达到池大小，等待直到有连接可用
 *    - 标记连接为活跃状态
 *
 *    使用连接：
 *    - 应用程序使用连接执行数据库操作
 *
 *    归还连接：
 *    - 应用程序使用完毕后归还连接
 *    - 检查连接是否有效
 *    - 将连接放回空闲队列
 *    - 通知等待的线程
 *
 * 4. 核心技术点：
 *    - 线程安全：使用互斥锁和条件变量
 *    - 连接管理：队列管理空闲和活跃连接
 *    - 健康检查：定期检查连接状态
 *    - 自动重连：处理连接失效的情况
 *    - 超时机制：防止线程无限等待
 *    - 智能配置：根据系统资源自动调整
 */

#include "auth/mysql_connection_pool.h"
#include <iostream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace reactor {

/**
 * @brief MySQL 连接封装构造函数
 */
MySQLConnection::MySQLConnection(MYSQL* conn)
    : conn_(conn) {}

/**
 * @brief MySQL 连接封装析构函数
 */
MySQLConnection::~MySQLConnection() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

/**
 * @brief 获取原始 MySQL 连接
 */
MYSQL* MySQLConnection::GetRawConnection() {
    return conn_;
}

/**
 * @brief 检查连接是否有效
 */
bool MySQLConnection::IsValid() {
    if (!conn_) {
        return false;
    }
    // 发送 ping 命令检查连接
    return mysql_ping(conn_) == 0;
}

/**
 * @brief 关闭连接
 */
void MySQLConnection::Close() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

/**
 * @brief 连接池构造函数
 */
MySQLConnectionPool::MySQLConnectionPool()
    : port_(3306), pool_size_(8), max_idle_time_(300),
      is_initialized_(false), active_count_(0) {}

/**
 * @brief 获取 CPU 核心数
 */
int GetCpuCoreCount() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/**
 * @brief 获取系统线程数
 */
int GetThreadCount() {
    return std::thread::hardware_concurrency();
}

/**
 * @brief 计算最优连接池大小
 */
int CalculateOptimalPoolSize() {
    int cpu_cores = GetCpuCoreCount();
    int thread_count = GetThreadCount();
    
    int size_by_threads = thread_count * 2;
    int size_by_cpu = cpu_cores * 4 + 1;
    
    return std::min(size_by_threads, size_by_cpu);
}

/**
 * @brief 连接池析构函数
 */
MySQLConnectionPool::~MySQLConnectionPool() {
    Close();
}

/**
 * @brief 初始化连接池
 */
void MySQLConnectionPool::Initialize(const std::string& host, const std::string& user,
                                   const std::string& password, const std::string& database,
                                   int port, int pool_size, int max_idle_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_initialized_) {
        return;
    }

    // 保存配置
    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;
    port_ = port;
    max_idle_time_ = max_idle_time;

    // 自动计算连接池大小
    if (pool_size <= 0) {
        pool_size_ = CalculateOptimalPoolSize();
        std::cout << "MySQL connection pool auto-calculated size: " << pool_size_ 
                  << " (CPU cores: " << GetCpuCoreCount() 
                  << ", threads: " << GetThreadCount() << ")" << std::endl;
    } else {
        pool_size_ = pool_size;
        std::cout << "MySQL connection pool using configured size: " << pool_size_ << std::endl;
    }

    // 预创建连接
    for (int i = 0; i < pool_size_; ++i) {
        MySQLConnection* conn = CreateConnection();
        if (conn) {
            connections_.push(conn);
        }
    }

    is_initialized_ = true;
    std::cout << "MySQL connection pool initialized with " << connections_.size() << " connections" << std::endl;
}

/**
 * @brief 创建新连接
 */
MySQLConnection* MySQLConnectionPool::CreateConnection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "MySQL initialization failed" << std::endl;
        return nullptr;
    }

    // 设置连接参数
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, "30");
    // 使用 MYSQL_OPT_RECONNECT 已被弃用，使用 mysql_ping 来检测连接状态
    // mysql_options(conn, MYSQL_OPT_RECONNECT, "1");

    // 连接数据库
    if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(),
                           password_.c_str(), database_.c_str(),
                           port_, nullptr, 0)) {
        std::cerr << "MySQL connection failed: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }

    // 设置字符集
    mysql_set_character_set(conn, "utf8mb4");

    return new MySQLConnection(conn);
}

/**
 * @brief 获取数据库连接
 */
MySQLConnection* MySQLConnectionPool::GetConnection(int timeout_ms) {
    if (!is_initialized_) {
        std::cerr << "MySQL connection pool not initialized" << std::endl;
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    // 等待连接可用
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);

    while (connections_.empty()) {
        if (active_count_ < pool_size_) {
            // 可以创建新连接
            MySQLConnection* conn = CreateConnection();
            if (conn) {
                active_count_++;
                return conn;
            }
        }

        // 等待有连接归还
        if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
            std::cerr << "MySQL connection timeout" << std::endl;
            return nullptr;
        }

        // 检查是否超时
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            std::cerr << "MySQL connection timeout" << std::endl;
            return nullptr;
        }
    }

    // 获取连接
    MySQLConnection* conn = connections_.front();
    connections_.pop();

    // 检查连接是否有效
    if (!CheckConnection(conn)) {
        delete conn;
        conn = CreateConnection();
        if (!conn) {
            active_count_++;
            return conn;
        }
    }

    active_count_++;
    return conn;
}

/**
 * @brief 归还数据库连接
 */
void MySQLConnectionPool::ReturnConnection(MySQLConnection* conn) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 检查连接是否有效
    if (CheckConnection(conn)) {
        connections_.push(conn);
    } else {
        delete conn;
    }

    active_count_--;
    cv_.notify_one();
}

/**
 * @brief 关闭连接池
 */
void MySQLConnectionPool::Close() {
    std::lock_guard<std::mutex> lock(mutex_);

    while (!connections_.empty()) {
        MySQLConnection* conn = connections_.front();
        connections_.pop();
        delete conn;
    }

    is_initialized_ = false;
    active_count_ = 0;
}

/**
 * @brief 获取当前空闲连接数
 */
int MySQLConnectionPool::GetIdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

/**
 * @brief 检查连接是否有效
 */
bool MySQLConnectionPool::CheckConnection(MySQLConnection* conn) {
    if (!conn) {
        return false;
    }
    return conn->IsValid();
}

/**
 * @brief 清理过期连接
 */
void MySQLConnectionPool::CleanExpiredConnections() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::queue<MySQLConnection*> valid_connections;

    while (!connections_.empty()) {
        MySQLConnection* conn = connections_.front();
        connections_.pop();

        if (CheckConnection(conn)) {
            valid_connections.push(conn);
        } else {
            delete conn;
        }
    }

    connections_ = valid_connections;
}

} // namespace reactor