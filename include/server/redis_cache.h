#pragma once

/**
 * @file redis_cache.h
 * @brief Redis 缓存外观层（Facade 模式）
 *
 * 设计模式：Facade（外观）
 * - 对外提供简洁的缓存 API（Set/Get/Delete/Exists/FlushAll）
 * - 内部委托给 RedisConnectionPool 处理连接管理
 * - 对上层（Auth 等业务模块）屏蔽底层连接细节
 *
 * 读写分离策略：
 * - 写操作（Set/Delete/FlushAll）→ 主库
 * - 读操作（Get/Exists）→ 优先从库，不可用时降级主库
 */

#include <string>
#include <vector>
#include <functional>
#include "auth/redis_connection_pool.h"

namespace reactor {

/**
 * @class RedisCache
 * @brief Redis 缓存外观层，单例模式
 *
 * 使用方式：
 * @code
 *   RedisCache::GetInstance().Initialize("localhost", 6379);
 *   RedisCache::GetInstance().Set("key", "value", 3600);
 *   std::string val;
 *   bool hit = RedisCache::GetInstance().Get("key", val);
 * @endcode
 */
class RedisCache {
public:
    static RedisCache& GetInstance() {
        static RedisCache instance;
        return instance;
    }

    void Initialize(const std::string& host, int port,
                    const std::string& password = "", int db = 0,
                    int pool_size = 10);

    void InitializeWithSlaves(const RedisNodeConfig& master_config,
                              const std::vector<RedisNodeConfig>& slave_configs = {});

    bool Set(const std::string& key, const std::string& value, int expire_seconds = 3600);
    bool Get(const std::string& key, std::string& value);
    bool Delete(const std::string& key);
    bool Exists(const std::string& key);
    bool FlushAll();

    bool HasSlaves() const;
    int GetSlaveCount() const;
    bool IsSlaveHealthy(int index) const;
    bool CheckMasterHealth();
    bool CheckSlaveHealth(int index);

    using FailoverCallback = std::function<void(const std::string& old_master, const std::string& new_master)>;
    void SetFailoverCallback(FailoverCallback callback);
    bool PerformFailover();
    bool EnsureMasterAvailable();
    bool BackupDatabase(const std::string& backup_path);

    using HealthAlertCallback = std::function<void(const std::string& node, bool is_healthy)>;
    void SetHealthAlertCallback(HealthAlertCallback callback);
    void StartHealthCheck(int interval_seconds = 10);
    void StopHealthCheck();

    int WaitForReplication(int num_slaves, int timeout_ms);

private:
    RedisCache();
    ~RedisCache();

    RedisCache(const RedisCache&) = delete;
    RedisCache& operator=(const RedisCache&) = delete;
};

} // namespace reactor
