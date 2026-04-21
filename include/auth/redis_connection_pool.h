#pragma once

/**
 * @file redis_connection_pool.h
 * @brief Redis 连接池模块
 *
 * 核心功能：
 * 1. 预创建多个 Redis 连接，避免频繁创建/销毁的开销
 * 2. 线程安全的连接获取和释放
 * 3. 连接健康检查和自动重连
 * 4. 支持配置池大小和超时时间
 */

#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>

namespace reactor {

/**
 * @class RedisConnection
 * @brief Redis 连接封装
 */
class RedisConnection {
public:
    RedisConnection(redisContext* conn);
    ~RedisConnection();

    redisContext* GetRawConnection();
    bool IsValid();
    void Close();

private:
    redisContext* conn_;
};

/**
 * @class RedisConnectionPool
 * @brief Redis 连接池
 */
class RedisConnectionPool {
public:
    /**
     * @brief 获取连接池单例
     */
    static RedisConnectionPool& GetInstance() {
        static RedisConnectionPool instance;
        return instance;
    }

    /**
     * @brief 初始化连接池
     * @param host Redis 主机地址
     * @param port Redis 端口
     * @param password Redis 密码
     * @param db Redis 数据库编号
     * @param pool_size 连接池大小
     * @param max_idle_time 最大空闲时间（秒）
     */
    void Initialize(const std::string& host, int port, const std::string& password = "", int db = 0, int pool_size = 10, int max_idle_time = 300);

    /**
     * @brief 获取Redis连接
     * @param timeout_ms 超时时间（毫秒）
     * @return RedisConnection* Redis连接
     */
    RedisConnection* GetConnection(int timeout_ms = 3000);

    /**
     * @brief 归还Redis连接
     * @param conn Redis连接
     */
    void ReturnConnection(RedisConnection* conn);

    /**
     * @brief 关闭连接池
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
    RedisConnectionPool();

    /**
     * @brief 析构函数
     */
    ~RedisConnectionPool();

    /**
     * @brief 创建新连接
     */
    RedisConnection* CreateConnection();

    /**
     * @brief 检查连接是否有效
     */
    bool CheckConnection(RedisConnection* conn);

    /**
     * @brief 清理过期连接
     */
    void CleanExpiredConnections();

private:
    // 连接池配置
    std::string host_;
    int port_;
    std::string password_;
    int db_;
    int pool_size_;
    int max_idle_time_;

    // 连接队列
    std::queue<RedisConnection*> connections_;

    // 线程安全相关
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 状态
    std::atomic<bool> is_initialized_;
    std::atomic<int> active_count_;
};

} // namespace reactor