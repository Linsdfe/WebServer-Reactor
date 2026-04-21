#pragma once

/**
 * @file mysql_connection_pool.h
 * @brief MySQL 连接池模块
 *
 * 核心功能：
 * 1. 预创建多个 MySQL 连接，避免频繁创建/销毁的开销
 * 2. 线程安全的连接获取和释放
 * 3. 连接健康检查和自动重连
 * 4. 支持配置池大小和超时时间
 *
 * 设计说明：
 * - 使用队列存储空闲连接
 * - 使用互斥锁保证线程安全
 * - 使用条件变量实现连接的等待和唤醒
 * - 定期检查连接健康状态
 */

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>

namespace reactor {

/**
 * @class MySQLConnection
 * @brief MySQL 连接封装
 *
 * 封装 MySQL 连接，提供基本的连接操作
 */
class MySQLConnection {
public:
    MySQLConnection(MYSQL* conn);
    ~MySQLConnection();

    MYSQL* GetRawConnection();
    bool IsValid();
    void Close();

private:
    MYSQL* conn_;
};

/**
 * @class MySQLConnectionPool
 * @brief MySQL 连接池
 *
 * 核心功能：
 * 1. 初始化连接池，创建指定数量的连接
 * 2. 线程安全地获取连接
 * 3. 线程安全地归还连接
 * 4. 连接健康检查
 * 5. 自动重连机制
 */
class MySQLConnectionPool {
public:
    /**
     * @brief 获取连接池单例
     */
    static MySQLConnectionPool& GetInstance() {
        static MySQLConnectionPool instance;
        return instance;
    }

    /**
     * @brief 初始化连接池
     * @param host MySQL 主机地址
     * @param user MySQL 用户名
     * @param password MySQL 密码
     * @param database 数据库名
     * @param port MySQL 端口
     * @param pool_size 连接池大小（0表示自动计算）
     * @param max_idle_time 最大空闲时间（秒）
     *
     * 连接池大小计算规则：
     * - pool_size > 0：使用指定的大小
     * - pool_size = 0：自动计算，公式为 min(线程数 * 2, CPU核心数 * 4 + 1)
     *   这个公式基于实践经验，既能保证并发性能，又不会过度消耗资源
     */
    void Initialize(const std::string& host, const std::string& user,
                   const std::string& password, const std::string& database,
                   int port = 3306, int pool_size = 0, int max_idle_time = 300);

    /**
     * @brief 获取数据库连接
     * @param timeout_ms 超时时间（毫秒）
     * @return MySQLConnection* 数据库连接
     *
     * 说明：
     * - 如果有空闲连接，直接返回
     * - 如果没有空闲连接且未达到池大小，创建新连接
     * - 如果达到池大小，等待直到有连接可用或超时
     */
    MySQLConnection* GetConnection(int timeout_ms = 3000);

    /**
     * @brief 归还数据库连接
     * @param conn 数据库连接
     */
    void ReturnConnection(MySQLConnection* conn);

    /**
     * @brief 关闭连接池
     * 
     * 清理所有连接
     */
    void Close();

    /**
     * @brief 获取连接池大小
     */
    int GetPoolSize() const { return pool_size_; }

    /**
     * @brief 获取当前空闲连接数
     */
    int GetIdleCount() const;

private:
    /**
     * @brief 构造函数
     */
    MySQLConnectionPool();

    /**
     * @brief 析构函数
     */
    ~MySQLConnectionPool();

    /**
     * @brief 创建新连接
     */
    MySQLConnection* CreateConnection();

    /**
     * @brief 检查连接是否有效
     */
    bool CheckConnection(MySQLConnection* conn);

    /**
     * @brief 清理过期连接
     */
    void CleanExpiredConnections();

private:
    // 连接池配置
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    int port_;
    int pool_size_;
    int max_idle_time_;

    // 连接队列
    std::queue<MySQLConnection*> connections_;

    // 线程安全相关
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 状态
    std::atomic<bool> is_initialized_;
    std::atomic<int> active_count_;
};

} // namespace reactor