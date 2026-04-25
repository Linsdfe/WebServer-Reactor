/**
 * @file metrics_collector.cpp
 * @brief Prometheus 格式性能指标收集器实现
 *
 * 核心功能：
 * 1. 收集 HTTP 请求指标（请求数、方法/路径分布、状态码、耗时直方图）
 * 2. 收集连接指标（总连接数、活跃连接数、读写字节数）
 * 3. 收集缓存指标（内存缓存和 Redis 缓存的命中/未命中/更新数）
 * 4. 收集主从复制指标（从库命中/降级/健康状态、复制延迟）
 * 5. 收集容灾指标（故障转移次数、备份次数、半同步状态）
 * 6. 导出 Prometheus 标准格式文本（/metrics 端点）
 *
 * ==================== 线程安全设计 ====================
 *
 * 一、原子变量（无锁，最高性能）
 *    - 所有计数器（total_requests_、cache_hits_ 等）使用 std::atomic<int64_t>
 *    - 使用 memory_order_relaxed 排序（监控指标无需严格顺序保证）
 *    - 适用于：递增/递减操作（fetch_add/fetch_sub）
 *
 * 二、互斥锁（细粒度，低竞争）
 *    - method_mutex_：保护 requests_by_method_（按方法分类的请求数）
 *    - path_mutex_：保护 requests_by_path_（按路径分类的请求数）
 *    - status_mutex_：保护 responses_by_status_（按状态码分类的响应数）
 *    - duration_mutex_：保护请求耗时直方图数据
 *    - slave_health_mutex_：保护从库健康状态和复制延迟
 *    - 锁粒度小，持有时间短，不影响核心业务性能
 *
 * 三、Prometheus 格式说明
 *    - Counter：只增不减的计数器（如总请求数）
 *    - Gauge：可增可减的仪表盘（如当前活跃连接数）
 *    - Histogram：分布统计（如请求耗时分布，含桶计数和分位数）
 */

#include "monitor/metrics_collector.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace reactor {

/**
 * @brief 获取 MetricsCollector 单例实例
 * @return MetricsCollector& 单例引用
 *
 * 使用 Meyers' Singleton 模式（C++11 保证线程安全）
 * static 局部变量在首次调用时初始化，且初始化是线程安全的
 */
MetricsCollector& MetricsCollector::Instance() {
    static MetricsCollector instance;
    return instance;
}

void MetricsCollector::IncTotalRequests() {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRequestsByMethod(const std::string& method) {
    std::lock_guard<std::mutex> lock(method_mutex_);
    requests_by_method_[method]++;
}

void MetricsCollector::IncRequestsByPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(path_mutex_);
    requests_by_path_[path]++;
}

void MetricsCollector::IncResponsesByStatus(int status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    responses_by_status_[status]++;
}

/**
 * @brief 记录请求耗时到直方图
 * @param seconds 请求耗时（秒）
 *
 * 将耗时值与预定义桶边界比较，递增对应桶的计数
 * 桶边界：0.0001, 0.0005, 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0
 * 用于计算 P50/P90/P99 等分位数延迟
 */
void MetricsCollector::RecordRequestDuration(double seconds) {
    std::lock_guard<std::mutex> lock(duration_mutex_);
    duration_count_++;
    duration_sum_ += seconds;
    for (size_t i = 0; i < kDurationBuckets.size(); i++) {
        if (seconds <= kDurationBuckets[i]) {
            duration_bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MetricsCollector::IncTotalConnections() {
    total_connections_.fetch_add(1, std::memory_order_relaxed);
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::DecTotalConnections() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void MetricsCollector::IncBytesRead(size_t bytes) {
    bytes_read_.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void MetricsCollector::IncBytesWritten(size_t bytes) {
    bytes_written_.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void MetricsCollector::IncCacheHits() {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncCacheMisses() {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMemoryCacheHits() {
    memory_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMemoryCacheMisses() {
    memory_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMemoryCacheUpdates() {
    memory_cache_updates_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisCacheHits() {
    redis_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisCacheMisses() {
    redis_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisCacheUpdates() {
    redis_cache_updates_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisCacheExpirations() {
    redis_cache_expirations_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisSlaveHits() {
    redis_slave_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisSlaveMisses() {
    redis_slave_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisSlaveFallbacks() {
    redis_slave_fallbacks_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::SetRedisSlaveCount(int count) {
    redis_slave_count_.store(static_cast<int64_t>(count), std::memory_order_relaxed);
}

void MetricsCollector::SetRedisSlaveHealthy(int index, bool healthy) {
    std::lock_guard<std::mutex> lock(slave_health_mutex_);
    redis_slave_healthy_[index] = healthy;
}

void MetricsCollector::UpdateCacheStats(size_t items, size_t size_bytes) {
    cache_items_.store(static_cast<int64_t>(items), std::memory_order_relaxed);
    cache_size_bytes_.store(static_cast<int64_t>(size_bytes), std::memory_order_relaxed);
}

// MySQL 监控指标实现

void MetricsCollector::IncMySQLSlaveHits() {
    mysql_slave_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMySQLSlaveMisses() {
    mysql_slave_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMySQLSlaveFallbacks() {
    mysql_slave_fallbacks_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::SetMySQLSlaveCount(int count) {
    mysql_slave_count_.store(static_cast<int64_t>(count), std::memory_order_relaxed);
}

void MetricsCollector::SetMySQLSlaveHealthy(int index, bool healthy) {
    std::lock_guard<std::mutex> lock(slave_health_mutex_);
    mysql_slave_healthy_[index] = healthy;
}

void MetricsCollector::UpdateMySQLPoolStats(int idle, int active, int pool_size, bool is_master) {
    if (is_master) {
        mysql_master_pool_idle_.store(static_cast<int64_t>(idle), std::memory_order_relaxed);
        mysql_master_pool_active_.store(static_cast<int64_t>(active), std::memory_order_relaxed);
        mysql_master_pool_size_.store(static_cast<int64_t>(pool_size), std::memory_order_relaxed);
    }
}

void MetricsCollector::UpdateMySQLReplicationLag(int slave_index, int64_t lag_ms) {
    std::lock_guard<std::mutex> lock(slave_health_mutex_);
    mysql_slave_replication_lag_[slave_index] = lag_ms;
}

void MetricsCollector::IncMySQLFailovers() {
    mysql_failovers_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncMySQLBackups() {
    mysql_backups_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::SetMySQLSemiSyncEnabled(bool enabled) {
    mysql_semi_sync_enabled_.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisFailovers() {
    redis_failovers_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::IncRedisBackups() {
    redis_backups_.fetch_add(1, std::memory_order_relaxed);
}

std::string MetricsCollector::FormatCounter(const std::string& name, int64_t value, const std::string& help) {
    std::ostringstream oss;
    oss << "# HELP " << name << " " << help << "\n";
    oss << "# TYPE " << name << " counter\n";
    oss << name << " " << value << "\n";
    return oss.str();
}

std::string MetricsCollector::FormatGauge(const std::string& name, double value, const std::string& help) {
    std::ostringstream oss;
    oss << "# HELP " << name << " " << help << "\n";
    oss << "# TYPE " << name << " gauge\n";
    oss << name << " " << std::fixed << std::setprecision(2) << value << "\n";
    return oss.str();
}

std::string MetricsCollector::FormatHistogram(const std::string& name, int64_t count, double sum,
                                              const std::vector<double>& buckets,
                                              const std::vector<int64_t>& bucket_counts,
                                              const std::string& help) {
    std::ostringstream oss;
    oss << "# HELP " << name << " " << help << "\n";
    oss << "# TYPE " << name << " histogram\n";
    int64_t cumulative = 0;
    for (size_t i = 0; i < buckets.size(); i++) {
        cumulative += bucket_counts[i];
        oss << name << "_bucket{le=\"" << std::fixed << std::setprecision(4) << buckets[i] << "\"} " << cumulative << "\n";
    }
    oss << name << "_bucket{le=\"+Inf\"} " << count << "\n";
    oss << name << "_sum " << std::fixed << std::setprecision(6) << sum << "\n";
    oss << name << "_count " << count << "\n";
    return oss.str();
}

/**
 * @brief 导出所有监控指标为 Prometheus 格式文本
 * @return std::string Prometheus 格式的指标文本
 *
 * 输出格式示例：
 * # HELP http_requests_total Total number of HTTP requests
 * # TYPE http_requests_total counter
 * http_requests_total 12345
 *
 * 包含的指标组：
 * 1. HTTP 请求指标（总数、方法分布、路径分布、状态码分布、耗时直方图）
 * 2. 连接指标（总连接数、活跃连接数、读写字节数）
 * 3. 缓存指标（内存缓存和 Redis 缓存统计）
 * 4. Redis 主从指标（从库命中/降级/健康状态）
 * 5. MySQL 主从指标（从库命中/降级/健康状态/复制延迟/连接池状态）
 * 6. 容灾指标（故障转移次数、备份次数、半同步状态）
 */
std::string MetricsCollector::ExportPrometheus() {
    std::ostringstream oss;

    oss << FormatCounter("http_requests_total", total_requests_.load(std::memory_order_relaxed),
                         "Total number of HTTP requests");

    {
        std::lock_guard<std::mutex> lock(method_mutex_);
        oss << "# HELP http_requests_by_method Total HTTP requests by method\n";
        oss << "# TYPE http_requests_by_method counter\n";
        for (const auto& [method, count] : requests_by_method_) {
            oss << "http_requests_by_method{method=\"" << method << "\"} " << count << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(path_mutex_);
        oss << "# HELP http_requests_by_path Total HTTP requests by path\n";
        oss << "# TYPE http_requests_by_path counter\n";
        for (const auto& [path, count] : requests_by_path_) {
            std::string sanitized = path;
            std::replace(sanitized.begin(), sanitized.end(), '"', '\'');
            oss << "http_requests_by_path{path=\"" << sanitized << "\"} " << count << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        oss << "# HELP http_responses_by_status Total HTTP responses by status code\n";
        oss << "# TYPE http_responses_by_status counter\n";
        for (const auto& [status, count] : responses_by_status_) {
            oss << "http_responses_by_status{status=\"" << status << "\"} " << count << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(duration_mutex_);
        std::vector<double> buckets(kDurationBuckets.begin(), kDurationBuckets.end());
        std::vector<int64_t> bucket_counts(buckets.size());
        for (size_t i = 0; i < buckets.size(); i++) {
            bucket_counts[i] = duration_bucket_counts_[i].load(std::memory_order_relaxed);
        }
        oss << FormatHistogram("http_request_duration_seconds", duration_count_, duration_sum_,
                               buckets, bucket_counts,
                               "HTTP request duration in seconds");
    }

    oss << FormatCounter("http_connections_total", total_connections_.load(std::memory_order_relaxed),
                         "Total number of connections accepted");

    oss << FormatGauge("http_active_connections", static_cast<double>(active_connections_.load(std::memory_order_relaxed)),
                       "Current number of active connections");

    oss << FormatCounter("http_bytes_read_total", bytes_read_.load(std::memory_order_relaxed),
                         "Total bytes read from connections");

    oss << FormatCounter("http_bytes_written_total", bytes_written_.load(std::memory_order_relaxed),
                         "Total bytes written to connections");

    oss << FormatCounter("cache_hits_total", cache_hits_.load(std::memory_order_relaxed),
                         "Total number of cache hits");

    oss << FormatCounter("cache_misses_total", cache_misses_.load(std::memory_order_relaxed),
                         "Total number of cache misses");

    oss << FormatGauge("cache_size_bytes", static_cast<double>(cache_size_bytes_.load(std::memory_order_relaxed)),
                       "Current cache size in bytes");

    oss << FormatGauge("cache_items", static_cast<double>(cache_items_.load(std::memory_order_relaxed)),
                       "Current number of cached items");

    oss << FormatCounter("memory_cache_hits_total", memory_cache_hits_.load(std::memory_order_relaxed),
                         "Total number of memory cache hits (GET requests)");

    oss << FormatCounter("memory_cache_misses_total", memory_cache_misses_.load(std::memory_order_relaxed),
                         "Total number of memory cache misses (GET requests)");

    oss << FormatCounter("memory_cache_updates_total", memory_cache_updates_.load(std::memory_order_relaxed),
                         "Total number of memory cache updates (GET requests)");

    oss << FormatCounter("redis_cache_hits_total", redis_cache_hits_.load(std::memory_order_relaxed),
                         "Total number of Redis cache hits (POST requests)");

    oss << FormatCounter("redis_cache_misses_total", redis_cache_misses_.load(std::memory_order_relaxed),
                         "Total number of Redis cache misses (POST requests)");

    oss << FormatCounter("redis_cache_updates_total", redis_cache_updates_.load(std::memory_order_relaxed),
                         "Total number of Redis cache updates (POST requests)");

    oss << FormatCounter("redis_cache_expirations_total", redis_cache_expirations_.load(std::memory_order_relaxed),
                         "Total number of Redis cache expirations (POST requests)");

    oss << FormatCounter("redis_slave_hits_total", redis_slave_hits_.load(std::memory_order_relaxed),
                         "Total number of Redis slave hits (read from slave)");

    oss << FormatCounter("redis_slave_misses_total", redis_slave_misses_.load(std::memory_order_relaxed),
                         "Total number of Redis slave misses");

    oss << FormatCounter("redis_slave_fallbacks_total", redis_slave_fallbacks_.load(std::memory_order_relaxed),
                         "Total number of Redis slave fallbacks to master");

    oss << FormatGauge("redis_slave_count", static_cast<double>(redis_slave_count_.load(std::memory_order_relaxed)),
                       "Number of configured Redis slave nodes");

    oss << FormatCounter("redis_failovers_total", redis_failovers_.load(std::memory_order_relaxed),
                         "Total number of Redis master failovers");

    oss << FormatCounter("redis_backups_total", redis_backups_.load(std::memory_order_relaxed),
                         "Total number of Redis database backups");

    {
        std::lock_guard<std::mutex> lock(slave_health_mutex_);
        oss << "# HELP redis_slave_healthy Redis slave node health status (1=healthy, 0=unhealthy)\n";
        oss << "# TYPE redis_slave_healthy gauge\n";
        for (const auto& [index, healthy] : redis_slave_healthy_) {
            oss << "redis_slave_healthy{slave=\"" << index << "\"} " << (healthy ? 1 : 0) << "\n";
        }
    }

    // ========== MySQL 监控指标 ==========

    oss << FormatCounter("mysql_slave_hits_total", mysql_slave_hits_.load(std::memory_order_relaxed),
                         "Total number of MySQL slave hits (read from slave)");

    oss << FormatCounter("mysql_slave_misses_total", mysql_slave_misses_.load(std::memory_order_relaxed),
                         "Total number of MySQL slave misses");

    oss << FormatCounter("mysql_slave_fallbacks_total", mysql_slave_fallbacks_.load(std::memory_order_relaxed),
                         "Total number of MySQL slave fallbacks to master");

    oss << FormatGauge("mysql_slave_count", static_cast<double>(mysql_slave_count_.load(std::memory_order_relaxed)),
                       "Number of configured MySQL slave nodes");

    oss << FormatGauge("mysql_master_pool_idle", static_cast<double>(mysql_master_pool_idle_.load(std::memory_order_relaxed)),
                       "Number of idle connections in MySQL master pool");

    oss << FormatGauge("mysql_master_pool_active", static_cast<double>(mysql_master_pool_active_.load(std::memory_order_relaxed)),
                       "Number of active connections in MySQL master pool");

    oss << FormatGauge("mysql_master_pool_size", static_cast<double>(mysql_master_pool_size_.load(std::memory_order_relaxed)),
                       "Total size of MySQL master connection pool");

    oss << FormatCounter("mysql_failovers_total", mysql_failovers_.load(std::memory_order_relaxed),
                         "Total number of MySQL master failovers");

    oss << FormatCounter("mysql_backups_total", mysql_backups_.load(std::memory_order_relaxed),
                         "Total number of MySQL database backups");

    oss << FormatGauge("mysql_semi_sync_enabled", static_cast<double>(mysql_semi_sync_enabled_.load(std::memory_order_relaxed)),
                       "Whether MySQL semi-sync replication is enabled (1=enabled, 0=disabled)");

    {
        std::lock_guard<std::mutex> lock(slave_health_mutex_);
        oss << "# HELP mysql_slave_healthy MySQL slave node health status (1=healthy, 0=unhealthy)\n";
        oss << "# TYPE mysql_slave_healthy gauge\n";
        for (const auto& [index, healthy] : mysql_slave_healthy_) {
            oss << "mysql_slave_healthy{slave=\"" << index << "\"} " << (healthy ? 1 : 0) << "\n";
        }

        oss << "# HELP mysql_slave_replication_lag_ms MySQL slave replication lag in milliseconds\n";
        oss << "# TYPE mysql_slave_replication_lag_ms gauge\n";
        for (const auto& [index, lag] : mysql_slave_replication_lag_) {
            oss << "mysql_slave_replication_lag_ms{slave=\"" << index << "\"} " << lag << "\n";
        }
    }

    return oss.str();
}

} // namespace reactor
