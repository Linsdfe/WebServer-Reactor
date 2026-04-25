#pragma once

/**
 * @file metrics_collector.h
 * @brief Prometheus 格式性能指标收集器
 *
 * 收集的指标类别：
 * 1. HTTP 请求指标（总数、方法/路径分布、状态码、耗时直方图）
 * 2. 连接指标（总连接数、活跃连接数、读写字节数）
 * 3. 缓存指标（内存缓存和 Redis 缓存的命中/未命中/更新数）
 * 4. 主从复制指标（从库命中/降级/健康状态、复制延迟）
 * 5. 容灾指标（故障转移次数、备份次数、半同步状态）
 *
 * 线程安全：所有计数器使用 std::atomic，映射表使用细粒度 std::mutex
 */

#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <vector>
#include <array>

namespace reactor {

/**
 * @class MetricsCollector
 * @brief 单例模式的 Prometheus 格式指标收集器
 *
 * 使用方式：
 * - MetricsCollector::Instance().IncTotalRequests();
 * - std::string metrics = MetricsCollector::Instance().ExportPrometheus();
 */
class MetricsCollector {
public:
    static MetricsCollector& Instance();

    void IncTotalRequests();
    void IncRequestsByMethod(const std::string& method);
    void IncRequestsByPath(const std::string& path);
    void IncResponsesByStatus(int status);
    void RecordRequestDuration(double seconds);
    void IncTotalConnections();
    void DecTotalConnections();
    void IncBytesRead(size_t bytes);
    void IncBytesWritten(size_t bytes);
    void IncCacheHits();
    void IncCacheMisses();
    void UpdateCacheStats(size_t items, size_t size_bytes);
    void IncMemoryCacheHits();
    void IncMemoryCacheMisses();
    void IncMemoryCacheUpdates();
    void IncRedisCacheHits();
    void IncRedisCacheMisses();
    void IncRedisCacheUpdates();
    void IncRedisCacheExpirations();
    void IncRedisSlaveHits();
    void IncRedisSlaveMisses();
    void IncRedisSlaveFallbacks();
    void SetRedisSlaveCount(int count);
    void SetRedisSlaveHealthy(int index, bool healthy);

    // MySQL 监控指标
    void IncMySQLSlaveHits();
    void IncMySQLSlaveMisses();
    void IncMySQLSlaveFallbacks();
    void SetMySQLSlaveCount(int count);
    void SetMySQLSlaveHealthy(int index, bool healthy);
    void UpdateMySQLPoolStats(int idle, int active, int pool_size, bool is_master);
    void UpdateMySQLReplicationLag(int slave_index, int64_t lag_ms);
    void IncMySQLFailovers();
    void IncMySQLBackups();
    void SetMySQLSemiSyncEnabled(bool enabled);

    void IncRedisFailovers();
    void IncRedisBackups();

    std::string ExportPrometheus();

private:
    MetricsCollector() = default;
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    std::string FormatCounter(const std::string& name, int64_t value, const std::string& help);
    std::string FormatGauge(const std::string& name, double value, const std::string& help);
    std::string FormatHistogram(const std::string& name, int64_t count, double sum,
                                const std::vector<double>& buckets,
                                const std::vector<int64_t>& bucket_counts,
                                const std::string& help);

    std::atomic<int64_t> total_requests_{0};
    std::atomic<int64_t> total_connections_{0};
    std::atomic<int64_t> active_connections_{0};
    std::atomic<int64_t> bytes_read_{0};
    std::atomic<int64_t> bytes_written_{0};
    std::atomic<int64_t> cache_hits_{0};
    std::atomic<int64_t> cache_misses_{0};
    std::atomic<int64_t> cache_items_{0};
    std::atomic<int64_t> cache_size_bytes_{0};
    std::atomic<int64_t> memory_cache_hits_{0};
    std::atomic<int64_t> memory_cache_misses_{0};
    std::atomic<int64_t> memory_cache_updates_{0};
    std::atomic<int64_t> redis_cache_hits_{0};
    std::atomic<int64_t> redis_cache_misses_{0};
    std::atomic<int64_t> redis_cache_updates_{0};
    std::atomic<int64_t> redis_cache_expirations_{0};
    std::atomic<int64_t> redis_slave_hits_{0};
    std::atomic<int64_t> redis_slave_misses_{0};
    std::atomic<int64_t> redis_slave_fallbacks_{0};
    std::atomic<int64_t> redis_slave_count_{0};

    // MySQL 监控指标
    std::atomic<int64_t> mysql_slave_hits_{0};
    std::atomic<int64_t> mysql_slave_misses_{0};
    std::atomic<int64_t> mysql_slave_fallbacks_{0};
    std::atomic<int64_t> mysql_slave_count_{0};
    std::atomic<int64_t> mysql_master_pool_idle_{0};
    std::atomic<int64_t> mysql_master_pool_active_{0};
    std::atomic<int64_t> mysql_master_pool_size_{0};
    std::atomic<int64_t> mysql_failovers_{0};
    std::atomic<int64_t> mysql_backups_{0};
    std::atomic<int64_t> mysql_semi_sync_enabled_{0};

    std::atomic<int64_t> redis_failovers_{0};
    std::atomic<int64_t> redis_backups_{0};

    std::mutex slave_health_mutex_;
    std::unordered_map<int, bool> redis_slave_healthy_;
    std::unordered_map<int, bool> mysql_slave_healthy_;
    std::unordered_map<int, int64_t> mysql_slave_replication_lag_;

    std::mutex method_mutex_;
    std::unordered_map<std::string, int64_t> requests_by_method_;

    std::mutex path_mutex_;
    std::unordered_map<std::string, int64_t> requests_by_path_;

    std::mutex status_mutex_;
    std::unordered_map<int, int64_t> responses_by_status_;

    std::mutex duration_mutex_;
    int64_t duration_count_{0};
    double duration_sum_{0.0};
    static constexpr std::array<double, 11> kDurationBuckets = {
        0.0001, 0.0005, 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0
    };
    std::array<std::atomic<int64_t>, 11> duration_bucket_counts_{};
};

} // namespace reactor
