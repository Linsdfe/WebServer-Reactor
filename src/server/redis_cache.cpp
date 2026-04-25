/**
 * @file redis_cache.cpp
 * @brief Redis 缓存层实现
 *
 * ==================== 设计模式：Facade（外观）====================
 *
 * RedisCache 是 RedisConnectionPool 的外观层（Wrapper）
 * - 对外提供简洁的缓存 API（Set/Get/Delete/Exists/FlushAll）
 * - 内部委托给 RedisConnectionPool 处理连接管理
 * - 对上层（Auth 等业务模块）屏蔽底层连接细节
 *
 * ==================== 读写分离策略 ====================
 *
 * 写操作（Set/Delete/FlushAll）
 *    → 始终使用主库连接（确保数据一致性）
 *    → 保证写操作立即生效
 *
 * 读操作（Get/Exists）
 *    → 优先使用从库连接（负载均衡）
 *    → 从库不可用时自动降级到主库
 *    → 更新命中/未命中监控指标
 *
 * ==================== 缓存失效机制 ====================
 *
 * 注册流程优化：
 *    AddUser 成功后：
 *    1. Delete("user:" + username)   → 删除旧缓存
 *    2. Set("user:" + username, ...) → 写入新缓存
 *    保证缓存与数据库的一致性
 */

#include "server/redis_cache.h"
#include "monitor/metrics_collector.h"
#include <hiredis/hiredis.h>
#include <cstring>

namespace reactor {

/**
 * @brief 构造函数
 *
 * 注意：RedisCache 是单例模式
 * - 私有构造函数（防止外部创建）
 * - 通过 GetInstance() 获取实例
 */
RedisCache::RedisCache() {
}

/**
 * @brief 析构函数
 */
RedisCache::~RedisCache() {
}

/**
 * @brief 初始化单机模式
 *
 * 步骤：
 * 1. 调用 RedisConnectionPool 单例的 Initialize
 * 2. 建立主库连接池
 */
void RedisCache::Initialize(const std::string& host, int port,
                           const std::string& password, int db, int pool_size) {
    RedisConnectionPool::GetInstance().Initialize(host, port, password, db, pool_size);
}

/**
 * @brief 初始化主从模式
 *
 * 步骤：
 * 1. 调用 RedisConnectionPool 单例的 InitializeWithSlaves
 * 2. 建立主库 + 从库连接池
 */
void RedisCache::InitializeWithSlaves(const RedisNodeConfig& master_config,
                                     const std::vector<RedisNodeConfig>& slave_configs) {
    RedisConnectionPool::GetInstance().InitializeWithSlaves(master_config, slave_configs);
}

/**
 * @brief 设置缓存值（写操作，始终走主库）
 *
 * 步骤：
 * 1. 从主库获取连接
 * 2. 根据过期时间执行 SET 或 SETEX 命令
 * 3. 归还连接到连接池
 *
 * @param key 缓存键
 * @param value 缓存值
 * @param expire_seconds 过期时间（秒），0表示永不过期
 * @return true设置成功，false设置失败
 */
bool RedisCache::Set(const std::string& key, const std::string& value, int expire_seconds) {
    // ========== 步骤1：获取主库连接（写操作必须走主库） ==========
    RedisConnection* conn = RedisConnectionPool::GetInstance().GetConnection();
    if (!conn) {
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        RedisConnectionPool::GetInstance().ReturnConnection(conn);
        return false;
    }

    // ========== 步骤2：执行 SET/SETEX 命令 ==========
    bool result = false;
    if (expire_seconds > 0) {
        // SETEX：设置值并指定过期时间
        redisReply* reply = (redisReply*)redisCommand(ctx, "SETEX %s %d %s",
                                                      key.c_str(), expire_seconds, value.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
            result = true;
        }
        freeReplyObject(reply);
    } else {
        // SET：设置值，不过期
        redisReply* reply = (redisReply*)redisCommand(ctx, "SET %s %s",
                                                      key.c_str(), value.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
            result = true;
        }
        freeReplyObject(reply);
    }

    // ========== 步骤3：归还连接 ==========
    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

/**
 * @brief 获取缓存值（读操作，优先走从库）
 *
 * 读操作流程：
 * 1. 判断是否配置了从库
 * 2. 优先从从库获取连接（GetSlaveConnection）
 * 3. 从库不可用时自动降级到主库
 * 4. 执行 GET 命令
 * 5. 更新命中/未命中监控指标
 *
 * @param key 缓存键
 * @param value 输出参数，存储获取的缓存值
 * @return true命中，false未命中
 */
bool RedisCache::Get(const std::string& key, std::string& value) {
    // ========== 步骤1：判断是否使用从库 ==========
    bool from_slave = RedisConnectionPool::GetInstance().HasSlaves();

    // ========== 步骤2：获取连接（从库或主库） ==========
    RedisConnection* conn = from_slave
        ? RedisConnectionPool::GetInstance().GetSlaveConnection()  // 优先从库
        : RedisConnectionPool::GetInstance().GetConnection();       // 无从库则主库

    // 获取连接失败
    if (!conn) {
        if (from_slave) {
            MetricsCollector::Instance().IncRedisSlaveMisses();
        }
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        if (from_slave) {
            RedisConnectionPool::GetInstance().ReturnSlaveConnection(conn);
            MetricsCollector::Instance().IncRedisSlaveMisses();
        } else {
            RedisConnectionPool::GetInstance().ReturnConnection(conn);
        }
        return false;
    }

    // ========== 步骤3：执行 GET 命令 ==========
    bool result = false;
    redisReply* reply = (redisReply*)redisCommand(ctx, "GET %s", key.c_str());
    if (reply && reply->type == REDIS_REPLY_STRING) {
        value = reply->str;
        result = true;
        // 更新从库命中指标
        if (from_slave) {
            MetricsCollector::Instance().IncRedisSlaveHits();
        }
    } else {
        // 更新从库未命中指标
        if (from_slave) {
            MetricsCollector::Instance().IncRedisSlaveMisses();
        }
    }
    freeReplyObject(reply);

    // ========== 步骤4：归还连接 ==========
    if (from_slave) {
        RedisConnectionPool::GetInstance().ReturnSlaveConnection(conn);
    } else {
        RedisConnectionPool::GetInstance().ReturnConnection(conn);
    }
    return result;
}

/**
 * @brief 删除缓存（写操作，始终走主库）
 *
 * 步骤：
 * 1. 从主库获取连接
 * 2. 执行 DEL 命令
 * 3. 归还连接
 *
 * @param key 缓存键
 * @return true删除成功，false删除失败
 */
bool RedisCache::Delete(const std::string& key) {
    RedisConnection* conn = RedisConnectionPool::GetInstance().GetConnection();
    if (!conn) {
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        RedisConnectionPool::GetInstance().ReturnConnection(conn);
        return false;
    }

    bool result = false;
    redisReply* reply = (redisReply*)redisCommand(ctx, "DEL %s", key.c_str());
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) {
        result = true;
    }
    freeReplyObject(reply);

    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

/**
 * @brief 检查键是否存在（读操作，优先走从库）
 */
bool RedisCache::Exists(const std::string& key) {
    bool from_slave = RedisConnectionPool::GetInstance().HasSlaves();
    RedisConnection* conn = from_slave
        ? RedisConnectionPool::GetInstance().GetSlaveConnection()
        : RedisConnectionPool::GetInstance().GetConnection();

    if (!conn) {
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        if (from_slave) {
            RedisConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        } else {
            RedisConnectionPool::GetInstance().ReturnConnection(conn);
        }
        return false;
    }

    bool result = false;
    redisReply* reply = (redisReply*)redisCommand(ctx, "EXISTS %s", key.c_str());
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) {
        result = true;
    }
    freeReplyObject(reply);

    if (from_slave) {
        RedisConnectionPool::GetInstance().ReturnSlaveConnection(conn);
    } else {
        RedisConnectionPool::GetInstance().ReturnConnection(conn);
    }
    return result;
}

/**
 * @brief 清空所有缓存（写操作，始终走主库）
 */
bool RedisCache::FlushAll() {
    RedisConnection* conn = RedisConnectionPool::GetInstance().GetConnection();
    if (!conn) {
        return false;
    }

    redisContext* ctx = conn->GetRawConnection();
    if (!ctx) {
        RedisConnectionPool::GetInstance().ReturnConnection(conn);
        return false;
    }

    bool result = false;
    redisReply* reply = (redisReply*)redisCommand(ctx, "FLUSHALL");
    if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
        result = true;
    }
    freeReplyObject(reply);

    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

// ==================== 以下是代理方法，委托给 RedisConnectionPool ====================

bool RedisCache::HasSlaves() const {
    return RedisConnectionPool::GetInstance().HasSlaves();
}

int RedisCache::GetSlaveCount() const {
    return RedisConnectionPool::GetInstance().GetSlavePoolCount();
}

bool RedisCache::IsSlaveHealthy(int index) const {
    return RedisConnectionPool::GetInstance().IsSlaveHealthy(index);
}

bool RedisCache::CheckMasterHealth() {
    return RedisConnectionPool::GetInstance().CheckMasterHealth();
}

bool RedisCache::CheckSlaveHealth(int index) {
    return RedisConnectionPool::GetInstance().CheckSlaveHealth(index);
}

/**
 * @brief 设置故障转移回调
 *
 * 当 Redis 主库发生故障转移时，调用此回调通知上层
 */
void RedisCache::SetFailoverCallback(FailoverCallback callback) {
    RedisConnectionPool::GetInstance().SetFailoverCallback(std::move(callback));
}

/**
 * @brief 执行主库故障转移
 */
bool RedisCache::PerformFailover() {
    return RedisConnectionPool::GetInstance().PerformFailover();
}

/**
 * @brief 确保主库可用
 */
bool RedisCache::EnsureMasterAvailable() {
    return RedisConnectionPool::GetInstance().EnsureMasterAvailable();
}

/**
 * @brief 备份 Redis 数据
 */
bool RedisCache::BackupDatabase(const std::string& backup_path) {
    return RedisConnectionPool::GetInstance().BackupDatabase(backup_path);
}

/**
 * @brief 设置健康检查告警回调
 */
void RedisCache::SetHealthAlertCallback(HealthAlertCallback callback) {
    RedisConnectionPool::GetInstance().SetHealthAlertCallback(std::move(callback));
}

/**
 * @brief 启动后台健康检查
 */
void RedisCache::StartHealthCheck(int interval_seconds) {
    RedisConnectionPool::GetInstance().StartHealthCheck(interval_seconds);
}

/**
 * @brief 停止后台健康检查
 */
void RedisCache::StopHealthCheck() {
    RedisConnectionPool::GetInstance().StopHealthCheck();
}

/**
 * @brief 等待从库确认复制
 *
 * 使用 Redis WAIT 命令，确保写操作被指定数量的从库确认
 */
int RedisCache::WaitForReplication(int num_slaves, int timeout_ms) {
    return RedisConnectionPool::GetInstance().WaitForReplication(num_slaves, timeout_ms);
}

} // namespace reactor
