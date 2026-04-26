/**
 * @file redis_connection_pool.cpp
 * @brief Redis 连接池实现
 *
 * 核心功能：
 * 1. 连接池管理（预创建、获取、归还、健康检查）
 * 2. 主从库架构（读写分离、轮询负载均衡、故障降级）
 * 3. 数据一致性（WAIT 命令确保复制）
 * 4. 容灾能力（主库故障自动转移、RDB 快照备份）
 * 5. 监控与告警（连接池状态、复制偏移量、健康检查）
 *
 * ==================== 架构设计 ====================
 *
 * 一、连接池结构
 *    RedisConnectionPool (主库连接池)
 *        ├── connections_ (queue<RedisConnection*>)  空闲连接队列
 *        ├── active_count_ (atomic<int>)              活跃连接计数
 *        ├── slave_pools_ (vector<SlavePool>)         从库连接池列表
 *        └── mutex_/cv_                              线程同步
 *
 *    SlavePool (从库连接池)
 *        ├── connections_ (queue<RedisConnection*>)  空闲连接队列
 *        ├── active_count_ (atomic<int>)              活跃连接计数
 *        ├── replication_offset (atomic<int64_t>)    复制偏移量
 *        └── is_healthy_ (atomic<bool>)               健康状态
 *
 * 二、与 MySQL 连接池的异同
 *    相同点：
 *    - 架构模式相同（主从库 + 读写分离）
 *    - 负载均衡策略相同（轮询）
 *    - 故障转移逻辑相同
 *
 *    不同点：
 *    - Redis 使用 hiredis 库（非官方 C 客户端）
 *    - 健康检查使用 PING 命令（Redis 特有）
 *    - 复制延迟通过 INFO replication 获取
 *    - 数据备份使用 BGSAVE（RDB 快照）
 *    - 等待复制使用 WAIT 命令
 */

#include "auth/redis_connection_pool.h"
#include "monitor/metrics_collector.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <memory>
#include <sstream>
#include <cstdlib>

namespace reactor {

// ==================== RedisConnection 封装类 ====================

/**
 * @brief Redis 连接封装构造函数
 * @param conn 已建立的 Redis 连接上下文
 */
RedisConnection::RedisConnection(redisContext* conn, const std::string& host, int port)
    : conn_(conn), host_(host), port_(port) {
}

/**
 * @brief 析构函数：关闭并释放 Redis 连接
 */
RedisConnection::~RedisConnection() {
    Close();
}

/**
 * @brief 获取底层 Redis 连接上下文
 */
redisContext* RedisConnection::GetRawConnection() {
    return conn_;
}

/**
 * @brief 检查连接是否有效
 * @return true 连接有效（无错误），false 连接有错误或为空
 *
 * 通过检查 conn_->err 是否为 0 来判断
 */
bool RedisConnection::IsValid() {
    return conn_ != nullptr && conn_->err == 0;
}

/**
 * @brief 主动关闭连接
 */
void RedisConnection::Close() {
    if (conn_) {
        redisFree(conn_);
        conn_ = nullptr;
    }
}

// ==================== RedisConnectionPool 构造函数/析构函数 ====================

/**
 * @brief 构造函数：初始化所有成员变量为默认值
 *
 * 默认值：
 * - port_: 6379 (Redis 默认端口)
 * - db_: 0 (默认数据库编号)
 * - master_pool_size_: 10 (主库连接池大小)
 * - max_idle_time_: 300秒
 * - is_initialized_: false
 * - active_count_: 0
 * - slave_round_robin_: 0
 * - master_healthy_: false
 * - failover_in_progress_: false
 * - health_check_running_: false
 */
RedisConnectionPool::RedisConnectionPool()
    : port_(6379),
      db_(0),
      master_pool_size_(10),
      max_idle_time_(300),
      is_initialized_(false),
      active_count_(0),
      slave_round_robin_(0),
      master_healthy_(false),
      failover_in_progress_(false),
      health_check_running_(false) {
}

/**
 * @brief 析构函数：停止健康检查，关闭所有连接
 */
RedisConnectionPool::~RedisConnectionPool() {
    StopHealthCheck();
    CloseAll();
}

// ==================== 初始化函数 ====================

/**
 * @brief 初始化单机模式 Redis 连接池
 *
 * 步骤：
 * 1. 检查是否已初始化
 * 2. 保存连接参数
 * 3. 预创建连接池大小的连接
 * 4. 更新健康状态
 *
 * @param host      Redis 主机地址
 * @param port      端口
 * @param password  密码（空表示无密码）
 * @param db        数据库编号
 * @param pool_size 连接池大小
 * @param max_idle_time 最大空闲时间
 */
void RedisConnectionPool::Initialize(const std::string& host, int port,
                                   const std::string& password, int db,
                                   int pool_size, int max_idle_time) {
    // ========== 步骤1：防止重复初始化 ==========
    if (is_initialized_) {
        return;
    }

    // ========== 步骤2：保存配置 ==========
    host_ = host;
    port_ = port;
    password_ = password;
    db_ = db;
    master_pool_size_ = pool_size;
    max_idle_time_ = max_idle_time;

    // ========== 步骤3：预创建连接 ==========
    std::lock_guard<std::mutex> lock(mutex_);
    int created = 0;
    for (int i = 0; i < master_pool_size_; ++i) {
        RedisConnection* conn = CreateConnection(host_, port_, password_, db_);
        if (conn && CheckConnection(conn)) {
            connections_.push(conn);
            created++;
        } else if (conn) {
            delete conn;
        }
    }

    std::cout << "[Redis-Master] Created " << created << "/" << master_pool_size_
              << " connections to " << host_ << ":" << port_ << std::endl;

    // ========== 步骤4：更新状态 ==========
    master_healthy_ = (created > 0);
    is_initialized_ = true;
}

/**
 * @brief 初始化主从模式 Redis 连接池
 *
 * 步骤：
 * 1. 初始化主库连接池
 * 2. 如果没有从库配置，直接返回
 * 3. 遍历从库配置，初始化每个从库
 * 4. 注册监控指标
 * 5. 刷新从库复制偏移量
 */
void RedisConnectionPool::InitializeWithSlaves(const RedisNodeConfig& master_config,
                                              const std::vector<RedisNodeConfig>& slave_configs) {
    // ========== 步骤1：初始化主库 ==========
    Initialize(master_config.host, master_config.port, master_config.password,
              master_config.db, master_config.pool_size, 300);

    // ========== 步骤2：检查从库 ==========
    if (slave_configs.empty()) {
        std::cout << "[Redis] No slave nodes configured, using master-only mode" << std::endl;
        return;
    }

    // ========== 步骤3：初始化从库 ==========
    int healthy_slaves = 0;
    for (size_t i = 0; i < slave_configs.size(); ++i) {
        slave_pools_.emplace_back(std::make_unique<SlavePool>());
        if (InitializeSlavePool(*slave_pools_[i], slave_configs[i], master_config)) {
            healthy_slaves++;
            std::cout << "[Redis-Slave-" << i << "] Connected to "
                      << slave_configs[i].host << ":" << slave_configs[i].port << std::endl;
        } else {
            std::cerr << "[Redis-Slave-" << i << "] Failed to connect to "
                      << slave_configs[i].host << ":" << slave_configs[i].port << std::endl;
        }
    }

    std::cout << "[Redis] Master-Slave replication: " << healthy_slaves
              << "/" << slave_configs.size() << " slaves healthy" << std::endl;

    // ========== 步骤4：注册监控 ==========
    MetricsCollector::Instance().SetRedisSlaveCount(static_cast<int>(slave_configs.size()));
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        MetricsCollector::Instance().SetRedisSlaveHealthy(static_cast<int>(i),
                                                         slave_pools_[i]->is_healthy.load());
    }

    // ========== 步骤5：刷新复制偏移量 ==========
    RefreshSlaveOffsets();
}

/**
 * @brief 初始化单个从库连接池
 * @param pool 从库池引用
 * @param config 从库配置
 * @param master_config 主库配置（用于自动配置主从复制）
 */
bool RedisConnectionPool::InitializeSlavePool(SlavePool& pool, const RedisNodeConfig& config,
                                               const RedisNodeConfig& master_config) {
    // ========== 保存配置 ==========
    pool.host = config.host;
    pool.port = config.port;
    pool.password = config.password;
    pool.db = config.db;
    pool.pool_size = config.pool_size;

    // ========== 预创建连接 ==========
    std::lock_guard<std::mutex> lock(pool.mutex);
    int created = 0;
    for (int i = 0; i < pool.pool_size; ++i) {
        RedisConnection* conn = CreateConnection(pool.host, pool.port, pool.password, pool.db);
        if (conn && CheckConnection(conn)) {
            // ========== 检查是否需要自动配置主从复制 ==========
            if (master_config.host.empty()) {
                pool.connections.push(conn);
                created++;
                continue;
            }

            redisContext* ctx = conn->GetRawConnection();
            redisReply* info_reply = (redisReply*)redisCommand(ctx, "INFO replication");
            if (info_reply && info_reply->type == REDIS_REPLY_STRING) {
                std::string info(info_reply->str, info_reply->len);
                bool is_master = info.find("role:master") != std::string::npos;

                if (is_master) {
                    std::cout << "[Redis-Slave] Node " << config.host << ":" << config.port
                              << " is master, configuring replication..." << std::endl;

                    // 执行 SLAVEOF 配置主从复制
                    std::string slaveof_cmd = "REPLICAOF " + master_config.host + " " + std::to_string(master_config.port);
                    redisReply* slaveof_reply = (redisReply*)redisCommand(ctx, slaveof_cmd.c_str());

                    if (slaveof_reply) {
                        if (slaveof_reply->type == REDIS_REPLY_STATUS &&
                            (strcmp(slaveof_reply->str, "OK") == 0 || strcmp(slaveof_reply->str, "BACKUP") == 0)) {
                            std::cout << "[Redis-Slave] Replication configured: "
                                      << config.host << ":" << config.port
                                      << " -> " << master_config.host << ":" << master_config.port << std::endl;
                        } else if (slaveof_reply->type == REDIS_REPLY_ERROR) {
                            std::cerr << "[Redis-Slave] REPLICAOF failed: " << slaveof_reply->str << std::endl;
                        }
                        freeReplyObject(slaveof_reply);
                    }

                    // 如果有密码，需要在主从复制后再次认证
                    if (!master_config.password.empty()) {
                        redisReply* auth_reply = (redisReply*)redisCommand(ctx, "AUTH %s", master_config.password.c_str());
                        if (auth_reply && auth_reply->type == REDIS_REPLY_ERROR) {
                            std::cerr << "[Redis-Slave] Auth failed: " << auth_reply->str << std::endl;
                        }
                        if (auth_reply) freeReplyObject(auth_reply);
                    }
                }
            }
            if (info_reply) freeReplyObject(info_reply);

            pool.connections.push(conn);
            created++;
        } else if (conn) {
            delete conn;
        }
    }

    pool.is_healthy = (created > 0);
    pool.is_initialized = true;
    return pool.is_healthy.load();
}

// ==================== 创建连接 ====================

/**
 * @brief 创建单个 Redis 连接
 *
 * 步骤：
 * 1. redisConnect() 建立 TCP 连接
 * 2. 检查连接错误
 * 3. 设置超时（3秒）
 * 4. 如果有密码，执行 AUTH 认证
 * 5. 如果 db 不为 0，执行 SELECT 切换数据库
 *
 * @param host     主机地址
 * @param port     端口
 * @param password 密码
 * @param db       数据库编号
 * @return RedisConnection* 包装后的连接
 */
RedisConnection* RedisConnectionPool::CreateConnection(const std::string& host, int port,
                                                    const std::string& password, int db) {
    // ========== 步骤1：建立 TCP 连接 ==========
    redisContext* ctx = redisConnect(host.c_str(), port);
    if (!ctx) {
        return nullptr;
    }
    if (ctx->err) {
        std::cerr << "[Redis] Connection error to " << host << ":" << port
                  << " - " << ctx->errstr << std::endl;
        redisFree(ctx);
        return nullptr;
    }

    // ========== 步骤2：设置超时 ==========
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    redisSetTimeout(ctx, tv);

    // ========== 步骤3：密码认证 ==========
    if (!password.empty()) {
        redisReply* reply = (redisReply*)redisCommand(ctx, "AUTH %s", password.c_str());
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    // ========== 步骤4：选择数据库 ==========
    if (db != 0) {
        redisReply* reply = (redisReply*)redisCommand(ctx, "SELECT %d", db);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    return new RedisConnection(ctx, host, port);
}

// ==================== 获取连接 ====================

/**
 * @brief 获取主库连接（用于写操作）
 *
 * 流程与 MySQL 类似：
 * 1. 检查初始化状态
 * 2. 检查主库健康
 * 3. 等待/创建连接
 * 4. 返回连接
 */
RedisConnection* RedisConnectionPool::GetConnection(int timeout_ms) {
    // ========== 步骤1：检查初始化 ==========
    if (!is_initialized_) {
        return nullptr;
    }

    // ========== 步骤2：检查主库健康 ==========
    if (!master_healthy_.load(std::memory_order_relaxed)) {
        if (!EnsureMasterAvailable()) {
            std::cerr << "[Redis] Master unavailable and failover failed" << std::endl;
            return nullptr;
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto timeout = std::chrono::milliseconds(timeout_ms);

    // ========== 步骤3：等待可用连接 ==========
    while (connections_.empty()) {
        if (active_count_ < master_pool_size_) {
            RedisConnection* conn = CreateConnection(host_, port_, password_, db_);
            if (conn) {
                active_count_++;
                return conn;
            }
        }

        if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
            return nullptr;
        }
    }

    // ========== 步骤4：获取连接 ==========
    RedisConnection* conn = connections_.front();
    connections_.pop();

    if (!conn->IsValid()) {
        delete conn;
        conn = CreateConnection(host_, port_, password_, db_);
    }

    active_count_++;
    return conn;
}

/**
 * @brief 获取从库连接（用于读操作）
 *
 * 负载均衡策略：
 * 1. 轮询选择从库
 * 2. 遍历找健康的从库
 * 3. 所有从库不可用，降级到主库
 */
RedisConnection* RedisConnectionPool::GetSlaveConnection(int timeout_ms) {
    // ========== 步骤1：无从库降级到主库 ==========
    if (slave_pools_.empty()) {
        return GetConnection(timeout_ms);
    }

    // ========== 步骤2：轮询选择 ==========
    uint32_t start_index = slave_round_robin_.fetch_add(1, std::memory_order_relaxed);
    uint32_t pool_count = static_cast<uint32_t>(slave_pools_.size());

    // ========== 步骤3：遍历找健康的从库 ==========
    for (uint32_t i = 0; i < pool_count; ++i) {
        uint32_t idx = (start_index + i) % pool_count;
        SlavePool& pool = *slave_pools_[idx];

        if (!pool.is_healthy.load(std::memory_order_relaxed)) {
            continue;
        }

        RedisConnection* conn = GetSlaveConnectionFromPool(pool, timeout_ms);
        if (conn) {
            return conn;
        }
    }

    // ========== 步骤4：降级到主库 ==========
    std::cout << "[Redis] All slaves unavailable, falling back to master" << std::endl;
    MetricsCollector::Instance().IncRedisSlaveFallbacks();
    return GetConnection(timeout_ms);
}

/**
 * @brief 从指定从库池获取连接
 */
RedisConnection* RedisConnectionPool::GetSlaveConnectionFromPool(SlavePool& pool, int timeout_ms) {
    std::unique_lock<std::mutex> lock(pool.mutex);
    auto timeout = std::chrono::milliseconds(timeout_ms);

    while (pool.connections.empty()) {
        if (pool.active_count.load() < pool.pool_size) {
            RedisConnection* conn = CreateConnection(pool.host, pool.port, pool.password, pool.db);
            if (conn) {
                pool.active_count++;
                return conn;
            }
        }

        if (pool.cv.wait_for(lock, timeout) == std::cv_status::timeout) {
            return nullptr;
        }
    }

    RedisConnection* conn = pool.connections.front();
    pool.connections.pop();

    if (!conn->IsValid()) {
        delete conn;
        conn = CreateConnection(pool.host, pool.port, pool.password, pool.db);
    }

    pool.active_count++;
    return conn;
}

// ==================== 归还连接 ====================

/**
 * @brief 归还主库连接
 */
void RedisConnectionPool::ReturnConnection(RedisConnection* conn) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (conn->IsValid()) {
        connections_.push(conn);
        cv_.notify_one();
    } else {
        delete conn;
    }
    active_count_--;
}

/**
 * @brief 归还从库连接
 *
 * 关键：通过连接的主机/端口信息匹配正确的从库池
 */
void RedisConnectionPool::ReturnSlaveConnection(RedisConnection* conn) {
    if (!conn) {
        return;
    }

    // ========== 步骤1：获取连接源地址 ==========
    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        delete conn;
        return;
    }

    // ========== 步骤2：匹配从库池 ==========
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        if (!pool.is_initialized.load()) continue;

        std::string conn_server_info = conn->GetHost() + ":" + std::to_string(conn->GetPort());

        std::string pool_server_info = pool.host + ":" + std::to_string(pool.port);
        // 地址匹配则归还到对应池
        if (conn_server_info == pool_server_info) {
            ReturnSlaveConnectionToPool(conn, pool);
            return;
        }
    }

    // ========== 步骤3：未匹配则归还到主库 ==========
    ReturnConnection(conn);
}

/**
 * @brief 归还连接到指定从库池
 */
void RedisConnectionPool::ReturnSlaveConnectionToPool(RedisConnection* conn, SlavePool& pool) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(pool.mutex);
    if (conn->IsValid()) {
        pool.connections.push(conn);
        pool.cv.notify_one();
    } else {
        delete conn;
    }
    pool.active_count--;
}

// ==================== 关闭和清理 ====================

/**
 * @brief 关闭主库连接池
 */
void RedisConnectionPool::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) {
        RedisConnection* conn = connections_.front();
        connections_.pop();
        delete conn;
    }
    is_initialized_ = false;
}

/**
 * @brief 关闭所有连接（主库 + 从库）
 */
void RedisConnectionPool::CloseAll() {
    StopHealthCheck();
    Close();

    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        std::lock_guard<std::mutex> lock(pool.mutex);
        while (!pool.connections.empty()) {
            RedisConnection* conn = pool.connections.front();
            pool.connections.pop();
            delete conn;
        }
        pool.is_initialized = false;
        pool.is_healthy = false;
    }
    slave_pools_.clear();
}

/**
 * @brief 获取主库空闲连接数
 */
int RedisConnectionPool::GetIdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

// ==================== 健康检查 ====================

/**
 * @brief 检查指定从库是否健康
 */
bool RedisConnectionPool::IsSlaveHealthy(int index) const {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return false;
    }
    return slave_pools_[index]->is_healthy.load(std::memory_order_relaxed);
}

/**
 * @brief 检查主库健康状态
 *
 * 使用 PING 命令检查
 */
bool RedisConnectionPool::CheckMasterHealth() {
    RedisConnection* conn = GetConnection(1000);
    if (!conn) {
        master_healthy_ = false;
        return false;
    }

    bool healthy = CheckConnection(conn);
    ReturnConnection(conn);
    master_healthy_ = healthy;
    return healthy;
}

/**
 * @brief 检查指定从库健康状态
 */
bool RedisConnectionPool::CheckSlaveHealth(int index) {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return false;
    }

    SlavePool& pool = *slave_pools_[index];
    RedisConnection* conn = GetSlaveConnectionFromPool(pool, 1000);
    if (!conn) {
        pool.is_healthy = false;
        return false;
    }

    bool healthy = CheckConnection(conn);
    ReturnSlaveConnectionToPool(conn, pool);
    pool.is_healthy = healthy;

    if (!healthy) {
        std::cerr << "[Redis-Slave-" << index << "] Health check failed for "
                  << pool.host << ":" << pool.port << std::endl;
    }

    return healthy;
}

/**
 * @brief 检查连接有效性
 *
 * 使用 PING 命令，返回 PONG 表示成功
 */
bool RedisConnectionPool::CheckConnection(RedisConnection* conn) {
    if (!conn || !conn->IsValid()) {
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    redisReply* reply = (redisReply*)redisCommand(ctx, "PING");
    if (!reply) {
        return false;
    }

    bool success = false;
    if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
        if (reply->str && (strcmp(reply->str, "PONG") == 0 || strcmp(reply->str, "OK") == 0)) {
            success = true;
        }
    }

    freeReplyObject(reply);
    return success;
}

// ==================== 数据一致性优化 ====================

/**
 * @brief 等待指定数量的从库确认复制
 *
 * Redis WAIT 命令：
 * - 阻塞等待直到 N 个从库确认收到了之前的写操作
 * - 用于实现强一致性保证
 *
 * @param num_slaves 等待的从库数量
 * @param timeout_ms 超时时间（毫秒）
 * @return 实际确认的从库数量，-1 表示失败
 */
int RedisConnectionPool::WaitForReplication(int num_slaves, int timeout_ms) {
    RedisConnection* conn = GetConnection(3000);
    if (!conn) {
        return -1;
    }

    redisContext* ctx = conn->GetRawConnection();
    redisReply* reply = (redisReply*)redisCommand(ctx, "WAIT %d %d", num_slaves, timeout_ms);

    int result = -1;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer;
    }

    if (reply) {
        freeReplyObject(reply);
    }
    ReturnConnection(conn);
    return result;
}

/**
 * @brief 获取从库复制偏移量
 */
int64_t RedisConnectionPool::GetSlaveReplicationOffset(int index) {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return -1;
    }
    return slave_pools_[index]->replication_offset.load(std::memory_order_relaxed);
}

/**
 * @brief 刷新所有从库的复制偏移量
 *
 * 执行 INFO replication 命令，解析从库的复制偏移量
 */
void RedisConnectionPool::RefreshSlaveOffsets() {
    RedisConnection* conn = GetConnection(3000);
    if (!conn) {
        return;
    }

    redisContext* ctx = conn->GetRawConnection();
    redisReply* reply = (redisReply*)redisCommand(ctx, "INFO replication");
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        ReturnConnection(conn);
        return;
    }

    // ========== 解析 INFO replication 输出 ==========
    std::string info(reply->str, reply->len);
    freeReplyObject(reply);
    ReturnConnection(conn);

    // 解析每个从库的 offset
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        std::string search_key = "slave" + std::to_string(i);
        size_t pos = info.find(search_key);
        if (pos != std::string::npos) {
            size_t offset_pos = info.find("offset=", pos);
            if (offset_pos != std::string::npos) {
                size_t start = offset_pos + 7;
                size_t end = info.find(',', start);
                if (end == std::string::npos) {
                    end = info.find('\n', start);
                }
                if (end != std::string::npos) {
                    std::string offset_str = info.substr(start, end - start);
                    pool.replication_offset.store(std::atoll(offset_str.c_str()), std::memory_order_relaxed);
                }
            }
        }
    }
}

// ==================== 容灾能力增强 ====================

/**
 * @brief 设置故障转移回调
 */
void RedisConnectionPool::SetFailoverCallback(FailoverCallback callback) {
    failover_callback_ = std::move(callback);
}

/**
 * @brief 执行主库故障转移
 *
 * 步骤：
 * 1. 加锁防止并发
 * 2. 选择复制偏移量最大的从库作为新主库
 * 3. 销毁旧主库连接
 * 4. 更新主库地址
 * 5. 创建新主库连接池
 * 6. 从从库列表移除被提升的从库
 * 7. 回调通知
 *
 * 注意：Redis 选择新主库的策略是选择复制偏移量最大的从库
 * （与 MySQL 的复制延迟策略不同）
 */
bool RedisConnectionPool::PerformFailover() {
    std::lock_guard<std::mutex> lock(failover_mutex_);

    // ========== 步骤1：防止并发 ==========
    if (failover_in_progress_.exchange(true)) {
        std::cerr << "[Redis] Failover already in progress" << std::endl;
        return false;
    }

    std::cout << "[Redis] Starting failover process..." << std::endl;

    // ========== 步骤2：选择新主库（偏移量最大） ==========
    int new_master_idx = SelectNewMaster();
    if (new_master_idx < 0) {
        std::cerr << "[Redis] No suitable slave found for failover" << std::endl;
        failover_in_progress_ = false;
        return false;
    }

    SlavePool& new_master_pool = *slave_pools_[new_master_idx];
    std::string old_master = host_ + ":" + std::to_string(port_);
    std::string new_master = new_master_pool.host + ":" + std::to_string(new_master_pool.port);

    std::cout << "[Redis] Promoting slave-" << new_master_idx << " (" << new_master << ") to master" << std::endl;

    // ========== 步骤3：销毁旧主库连接 ==========
    {
        std::lock_guard<std::mutex> pool_lock(mutex_);
        while (!connections_.empty()) {
            RedisConnection* conn = connections_.front();
            connections_.pop();
            delete conn;
        }
    }

    // ========== 步骤4：更新主库地址 ==========
    host_ = new_master_pool.host;
    port_ = new_master_pool.port;

    // ========== 步骤5：创建新主库连接池 ==========
    {
        std::lock_guard<std::mutex> pool_lock(mutex_);
        for (int i = 0; i < master_pool_size_; ++i) {
            RedisConnection* conn = CreateConnection(host_, port_, password_, db_);
            if (conn) {
                connections_.push(conn);
            }
        }
        master_healthy_ = !connections_.empty();
    }

    // ========== 步骤6：移除被提升的从库 ==========
    slave_pools_.erase(slave_pools_.begin() + new_master_idx);

    // ========== 步骤7：回调通知 ==========
    if (failover_callback_) {
        failover_callback_(old_master, new_master);
    }

    MetricsCollector::Instance().IncRedisFailovers();
    std::cout << "[Redis] Failover completed: " << old_master << " -> " << new_master << std::endl;
    failover_in_progress_ = false;
    return true;
}

/**
 * @brief 确保主库可用
 */
bool RedisConnectionPool::EnsureMasterAvailable() {
    if (master_healthy_.load(std::memory_order_relaxed)) {
        return true;
    }

    if (CheckMasterHealth()) {
        return true;
    }

    std::cerr << "[Redis] Master is unavailable, attempting failover..." << std::endl;
    return PerformFailover();
}

/**
 * @brief 选择最佳从库作为新主库
 *
 * 选择策略：复制偏移量最大的健康从库
 * （数据最完整）
 */
int RedisConnectionPool::SelectNewMaster() {
    int best_idx = -1;
    int64_t max_offset = -1;

    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];

        if (!pool.is_healthy.load(std::memory_order_relaxed)) {
            continue;
        }

        int64_t offset = pool.replication_offset.load(std::memory_order_relaxed);
        if (offset > max_offset) {
            max_offset = offset;
            best_idx = static_cast<int>(i);
        }
    }

    return best_idx;
}

/**
 * @brief 备份 Redis 数据
 *
 * 使用 BGSAVE 命令触发后台 RDB 快照
 * 注意：BGSAVE 是异步的，不等待备份完成
 */
bool RedisConnectionPool::BackupDatabase(const std::string& backup_path) {
    RedisConnection* conn = GetConnection(3000);
    if (!conn) {
        std::cerr << "[Redis] Cannot get connection for backup" << std::endl;
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    redisReply* reply = (redisReply*)redisCommand(ctx, "BGSAVE");
    bool success = false;

    if (reply && (reply->type == REDIS_REPLY_STATUS || reply->type == REDIS_REPLY_STRING)) {
        std::cout << "[Redis] Background save initiated" << std::endl;
        success = true;
    } else {
        std::cerr << "[Redis] BGSAVE command failed" << std::endl;
    }

    if (reply) {
        freeReplyObject(reply);
    }
    ReturnConnection(conn);

    if (success) {
        std::cout << "[Redis] Database backup initiated (BGSAVE)" << std::endl;
        MetricsCollector::Instance().IncRedisBackups();
    }

    return success;
}

// ==================== 监控与告警 ====================

/**
 * @brief 获取主库连接池统计
 */
RedisPoolStats RedisConnectionPool::GetMasterPoolStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RedisPoolStats stats;
    stats.idle_count = static_cast<int>(connections_.size());
    stats.active_count = active_count_.load(std::memory_order_relaxed);
    stats.pool_size = master_pool_size_;
    stats.is_healthy = master_healthy_.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief 获取从库连接池统计
 */
RedisPoolStats RedisConnectionPool::GetSlavePoolStats(int index) const {
    RedisPoolStats stats = {0, 0, 0, false};
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return stats;
    }

    const SlavePool& pool = *slave_pools_[index];
    std::lock_guard<std::mutex> lock(pool.mutex);
    stats.idle_count = static_cast<int>(pool.connections.size());
    stats.active_count = pool.active_count.load(std::memory_order_relaxed);
    stats.pool_size = pool.pool_size;
    stats.is_healthy = pool.is_healthy.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief 设置健康检查告警回调
 */
void RedisConnectionPool::SetHealthAlertCallback(HealthAlertCallback callback) {
    health_alert_callback_ = std::move(callback);
}

/**
 * @brief 启动后台健康检查线程
 */
void RedisConnectionPool::StartHealthCheck(int interval_seconds) {
    if (health_check_running_.load()) {
        return;
    }

    health_check_running_ = true;
    health_check_thread_ = std::thread(&RedisConnectionPool::HealthCheckLoop, this, interval_seconds);
    std::cout << "[Redis] Health check started with interval " << interval_seconds << "s" << std::endl;
}

/**
 * @brief 停止后台健康检查线程
 */
void RedisConnectionPool::StopHealthCheck() {
    health_check_running_ = false;
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }
}

/**
 * @brief 健康检查线程主循环
 *
 * 每隔 interval_seconds 执行：
 * 1. 检查主库健康
 * 2. 检查所有从库健康
 * 3. 刷新从库复制偏移量
 * 4. 睡眠等待
 */
void RedisConnectionPool::HealthCheckLoop(int interval_seconds) {
    while (health_check_running_.load()) {
        // ========== 检查主库 ==========
        bool master_ok = CheckMasterHealth();
        if (!master_ok) {
            std::cerr << "[Redis-ALERT] Master health check failed!" << std::endl;
            if (health_alert_callback_) {
                std::string node = host_ + ":" + std::to_string(port_);
                health_alert_callback_(node, false);
            }
        }

        // ========== 检查从库 ==========
        for (size_t i = 0; i < slave_pools_.size(); ++i) {
            bool slave_ok = CheckSlaveHealth(static_cast<int>(i));
            MetricsCollector::Instance().SetRedisSlaveHealthy(static_cast<int>(i), slave_ok);

            if (!slave_ok && health_alert_callback_) {
                SlavePool& pool = *slave_pools_[i];
                std::string node = pool.host + ":" + std::to_string(pool.port);
                health_alert_callback_(node, false);
            }
        }

        // ========== 刷新复制偏移量 ==========
        RefreshSlaveOffsets();

        // ========== 睡眠等待 ==========
        for (int i = 0; i < interval_seconds && health_check_running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::cout << "[Redis] Health check stopped" << std::endl;
}

} // namespace reactor

