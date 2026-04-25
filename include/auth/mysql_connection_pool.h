#pragma once

/**
 * @file mysql_connection_pool.h
 * @brief MySQL 连接池：支持主从复制、读写分离、自动故障转移
 *
 * 核心功能：
 * 1. 主库连接池管理（写操作）
 * 2. 从库连接池管理（读操作，轮询负载均衡）
 * 3. 半同步复制支持（数据一致性保障）
 * 4. 复制延迟监控（超过阈值自动跳过该从库）
 * 5. 自动故障转移（主库不可用时提升从库为新主库）
 * 6. 后台健康检查线程（定期检测主从库状态）
 * 7. 数据备份（mysqldump）
 */

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace reactor {

struct MySQLNodeConfig {
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string database;
    int pool_size;
    bool is_master;

    MySQLNodeConfig()
        : port(3306), pool_size(0), is_master(true) {}

    MySQLNodeConfig(const std::string& h, int p, const std::string& u,
                    const std::string& pwd, const std::string& db, int ps = 0, bool master = true)
        : host(h), port(p), user(u), password(pwd), database(db), pool_size(ps), is_master(master) {}
};

class MySQLConnection {
public:
    MySQLConnection(MYSQL* conn, const std::string& host = "", int port = 0);
    ~MySQLConnection();

    MYSQL* GetRawConnection();
    bool IsValid();
    void Close();

    const std::string& GetHost() const { return host_; }
    int GetPort() const { return port_; }

private:
    MYSQL* conn_;
    std::string host_;
    int port_;
};

struct SlavePool {
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string database;
    int pool_size;
    std::queue<MySQLConnection*> connections;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<int> active_count;
    std::atomic<bool> is_initialized;
    std::atomic<bool> is_healthy;
    std::atomic<int64_t> replication_lag_ms;

    SlavePool() : port(3306), pool_size(0), active_count(0),
                  is_initialized(false), is_healthy(false), replication_lag_ms(0) {}
};

struct PoolStats {
    int idle_count;
    int active_count;
    int pool_size;
    bool is_healthy;
};

class MySQLConnectionPool {
public:
    static MySQLConnectionPool& GetInstance() {
        static MySQLConnectionPool instance;
        return instance;
    }

    void Initialize(const std::string& host, const std::string& user,
                   const std::string& password, const std::string& database,
                   int port = 3306, int pool_size = 0, int max_idle_time = 300);

    void InitializeWithSlaves(const MySQLNodeConfig& master_config,
                             const std::vector<MySQLNodeConfig>& slave_configs,
                             int max_idle_time = 300);

    MySQLConnection* GetMasterConnection(int timeout_ms = 3000);
    MySQLConnection* GetSlaveConnection(int timeout_ms = 3000);
    MySQLConnection* GetConnection(int timeout_ms = 3000);

    void ReturnMasterConnection(MySQLConnection* conn);
    void ReturnSlaveConnection(MySQLConnection* conn);
    void ReturnConnection(MySQLConnection* conn);

    void Close();

    int GetPoolSize() const { return pool_size_; }
    int GetIdleCount() const;
    bool HasSlaves() const { return !slave_pools_.empty(); }
    int GetSlavePoolCount() const { return slave_pools_.size(); }
    bool IsSlaveHealthy(int index) const;
    bool CheckMasterHealth();
    bool CheckSlaveHealth(int index);

    // ========== 数据一致性优化 ==========

    /**
     * @brief 配置半同步复制
     * @param timeout_ms 半同步复制超时时间（毫秒），0表示禁用
     *
     * 半同步复制确保主库写操作至少被一个从库接收后才返回
     * 超时后自动降级为异步复制，避免主库阻塞
     */
    void EnableSemiSync(int timeout_ms = 3000);

    /**
     * @brief 获取从库复制延迟（毫秒）
     * @param index 从库索引
     * @return 复制延迟（毫秒），-1表示获取失败
     */
    int64_t GetSlaveReplicationLag(int index) const;

    /**
     * @brief 刷新所有从库的复制延迟信息
     */
    void RefreshReplicationLag();

    // ========== 容灾能力增强 ==========

    using FailoverCallback = std::function<void(const std::string& old_master, const std::string& new_master)>;

    /**
     * @brief 设置主库故障转移回调
     * @param callback 回调函数，参数为旧主库地址和新主库地址
     */
    void SetFailoverCallback(FailoverCallback callback);

    /**
     * @brief 执行主库故障转移
     * @return true转移成功，false转移失败
     *
     * 策略：
     * 1. 检测主库不可用
     * 2. 选择复制延迟最小的从库提升为新主库
     * 3. 重新配置其他从库指向新主库
     * 4. 更新连接池配置
     */
    bool PerformFailover();

    /**
     * @brief 检查主库是否可用，不可用时自动触发故障转移
     * @return true主库可用，false主库不可用且故障转移失败
     */
    bool EnsureMasterAvailable();

    /**
     * @brief 备份数据库
     * @param backup_path 备份文件路径
     * @return true备份成功，false备份失败
     */
    bool BackupDatabase(const std::string& backup_path);

    // ========== 监控与告警 ==========

    /**
     * @brief 获取主库连接池统计信息
     */
    PoolStats GetMasterPoolStats() const;

    /**
     * @brief 获取指定从库连接池统计信息
     */
    PoolStats GetSlavePoolStats(int index) const;

    /**
     * @brief 获取所有从库的复制延迟摘要
     * @return 每个从库的复制延迟（毫秒）
     */
    std::vector<int64_t> GetAllReplicationLags() const;

    /**
     * @brief 设置复制延迟告警阈值
     * @param threshold_ms 延迟阈值（毫秒），超过此值触发告警
     */
    void SetReplicationLagAlert(int64_t threshold_ms);

    /**
     * @brief 设置健康检查回调
     * @param callback 回调函数，参数为节点地址和是否健康
     */
    using HealthAlertCallback = std::function<void(const std::string& node, bool is_healthy)>;
    void SetHealthAlertCallback(HealthAlertCallback callback);

    /**
     * @brief 启动后台健康检查线程
     * @param interval_seconds 检查间隔（秒）
     */
    void StartHealthCheck(int interval_seconds = 10);

    /**
     * @brief 停止后台健康检查线程
     */
    void StopHealthCheck();

private:
    MySQLConnectionPool();
    ~MySQLConnectionPool();

    MySQLConnection* CreateConnection(const std::string& host, int port,
                                    const std::string& user, const std::string& password,
                                    const std::string& database);
    bool CheckConnection(MySQLConnection* conn);
    void CleanExpiredConnections();
    bool InitializeSlavePool(SlavePool& pool, const MySQLNodeConfig& config);
    MySQLConnection* GetSlaveConnectionFromPool(SlavePool& pool, int timeout_ms);
    void ReturnSlaveConnectionToPool(MySQLConnection* conn, SlavePool& pool);

    /**
     * @brief 从指定从库查询复制延迟
     */
    int64_t QueryReplicationLag(SlavePool& pool);

    /**
     * @brief 后台健康检查线程函数
     */
    void HealthCheckLoop(int interval_seconds);

    /**
     * @brief 选择最佳从库提升为新主库
     * @return 被选中的从库索引，-1表示无可用从库
     */
    int SelectNewMaster();

private:
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    int port_;
    int pool_size_;
    int max_idle_time_;

    std::queue<MySQLConnection*> connections_;

    std::vector<std::unique_ptr<SlavePool>> slave_pools_;
    std::atomic<uint32_t> slave_round_robin_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::atomic<bool> is_initialized_;
    std::atomic<int> active_count_;
    std::atomic<bool> master_healthy_;

    // 数据一致性
    std::atomic<bool> semi_sync_enabled_;
    std::atomic<int> semi_sync_timeout_ms_;

    // 容灾
    FailoverCallback failover_callback_;
    std::mutex failover_mutex_;
    std::atomic<bool> failover_in_progress_;

    // 监控与告警
    std::atomic<int64_t> replication_lag_alert_threshold_ms_;
    HealthAlertCallback health_alert_callback_;
    std::thread health_check_thread_;
    std::atomic<bool> health_check_running_;
};

} // namespace reactor
