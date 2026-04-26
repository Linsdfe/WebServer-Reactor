#pragma once

/**
 * @file redis_connection_pool.h
 * @brief Redis 连接池：支持主从复制、读写分离、自动故障转移
 *
 * 核心功能：
 * 1. 主库连接池管理（写操作）
 * 2. 从库连接池管理（读操作，轮询负载均衡）
 * 3. WAIT 命令强一致性（确保写操作被从库确认）
 * 4. 复制偏移量监控（选择数据最完整的从库）
 * 5. 自动故障转移（主库不可用时提升偏移量最大的从库）
 * 6. 后台健康检查线程（PING 命令检测）
 * 7. 数据备份（BGSAVE RDB 快照）
 */

#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <thread>

namespace reactor {

class RedisConnection {
public:
    explicit RedisConnection(redisContext* conn, const std::string& host = "", int port = 0);
    ~RedisConnection();

    redisContext* GetRawConnection();
    bool IsValid();
    void Close();
    std::string GetHost() const { return host_; }
    int GetPort() const { return port_; }

private:
    redisContext* conn_;
    std::string host_;
    int port_;
};

struct RedisNodeConfig {
    std::string host;
    int port;
    std::string password;
    int db;
    int pool_size;

    RedisNodeConfig()
        : port(6379), db(0), pool_size(10) {}

    RedisNodeConfig(const std::string& h, int p, const std::string& pwd = "", int d = 0, int ps = 10)
        : host(h), port(p), password(pwd), db(d), pool_size(ps) {}
};

struct RedisPoolStats {
    int idle_count;
    int active_count;
    int pool_size;
    bool is_healthy;
};

class RedisConnectionPool {
public:
    static RedisConnectionPool& GetInstance() {
        static RedisConnectionPool instance;
        return instance;
    }

    void Initialize(const std::string& host, int port,
                    const std::string& password = "", int db = 0,
                    int pool_size = 10, int max_idle_time = 300);

    void InitializeWithSlaves(const RedisNodeConfig& master_config,
                              const std::vector<RedisNodeConfig>& slave_configs = {});

    RedisConnection* GetConnection(int timeout_ms = 3000);
    RedisConnection* GetSlaveConnection(int timeout_ms = 3000);
    void ReturnConnection(RedisConnection* conn);
    void ReturnSlaveConnection(RedisConnection* conn);

    void Close();
    void CloseAll();

    int GetPoolSize() const { return master_pool_size_; }
    int GetIdleCount() const;
    int GetSlavePoolCount() const { return static_cast<int>(slave_pools_.size()); }
    bool HasSlaves() const { return !slave_pools_.empty(); }
    bool IsSlaveHealthy(int index) const;

    bool CheckMasterHealth();
    bool CheckSlaveHealth(int index);

    // ========== 数据一致性优化 ==========

    /**
     * @brief 等待主库写操作同步到从库（WAIT命令）
     * @param num_slaves 等待的从库数量
     * @param timeout_ms 超时时间（毫秒）
     * @return 实际同步的从库数量，-1表示失败
     *
     * Redis WAIT命令确保写操作被指定数量的从库接收
     * 用于需要强一致性的场景
     */
    int WaitForReplication(int num_slaves, int timeout_ms);

    /**
     * @brief 获取从库复制偏移量差异
     * @param index 从库索引
     * @return 复制偏移量差异，-1表示获取失败
     */
    int64_t GetSlaveReplicationOffset(int index);

    // ========== 容灾能力增强 ==========

    using FailoverCallback = std::function<void(const std::string& old_master, const std::string& new_master)>;

    /**
     * @brief 设置主库故障转移回调
     */
    void SetFailoverCallback(FailoverCallback callback);

    /**
     * @brief 执行主库故障转移
     * @return true转移成功，false转移失败
     */
    bool PerformFailover();

    /**
     * @brief 确保主库可用，不可用时自动触发故障转移
     */
    bool EnsureMasterAvailable();

    /**
     * @brief 备份Redis数据（RDB快照）
     * @param backup_path 备份文件路径
     * @return true备份成功，false备份失败
     */
    bool BackupDatabase(const std::string& backup_path);

    // ========== 监控与告警 ==========

    /**
     * @brief 获取主库连接池统计信息
     */
    RedisPoolStats GetMasterPoolStats() const;

    /**
     * @brief 获取指定从库连接池统计信息
     */
    RedisPoolStats GetSlavePoolStats(int index) const;

    using HealthAlertCallback = std::function<void(const std::string& node, bool is_healthy)>;
    void SetHealthAlertCallback(HealthAlertCallback callback);

    /**
     * @brief 启动后台健康检查线程
     */
    void StartHealthCheck(int interval_seconds = 10);

    /**
     * @brief 停止后台健康检查线程
     */
    void StopHealthCheck();

private:
    RedisConnectionPool();
    ~RedisConnectionPool();

    RedisConnection* CreateConnection(const std::string& host, int port,
                                       const std::string& password, int db);
    bool CheckConnection(RedisConnection* conn);

    struct SlavePool {
        std::string host;
        int port;
        std::queue<RedisConnection*> connections;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::atomic<int> active_count;
        std::atomic<bool> is_healthy;
        std::atomic<bool> is_initialized;
        int pool_size;
        std::string password;
        int db;
        std::atomic<int64_t> replication_offset;

        SlavePool() : port(6379), active_count(0), is_healthy(false),
                      is_initialized(false), pool_size(10), db(0), replication_offset(0) {}
    };

    bool InitializeSlavePool(SlavePool& pool, const RedisNodeConfig& config,
                            const RedisNodeConfig& master_config = RedisNodeConfig());
    RedisConnection* GetSlaveConnectionFromPool(SlavePool& pool, int timeout_ms);
    void ReturnSlaveConnectionToPool(RedisConnection* conn, SlavePool& pool);

    int SelectNewMaster();
    void HealthCheckLoop(int interval_seconds);
    void RefreshSlaveOffsets();

private:
    std::string host_;
    int port_;
    std::string password_;
    int db_;
    int master_pool_size_;
    int max_idle_time_;

    std::queue<RedisConnection*> connections_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::atomic<bool> is_initialized_;
    std::atomic<int> active_count_;

    std::vector<std::unique_ptr<SlavePool>> slave_pools_;
    std::atomic<uint32_t> slave_round_robin_;
    std::atomic<bool> master_healthy_;

    FailoverCallback failover_callback_;
    std::mutex failover_mutex_;
    std::atomic<bool> failover_in_progress_;

    HealthAlertCallback health_alert_callback_;
    std::thread health_check_thread_;
    std::atomic<bool> health_check_running_;
};

} // namespace reactor

