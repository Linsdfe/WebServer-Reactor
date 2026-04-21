#pragma once

/**
 * @file redis_cache.h
 * @brief Redis缓存管理器
 *
 * 核心功能：
 * 1. 提供基于Redis的缓存操作
 * 2. 支持缓存设置、获取、删除
 * 3. 支持缓存过期时间设置
 * 4. 线程安全的缓存访问
 */

#include <string>
#include <mutex>
#include "auth/redis_connection_pool.h"

namespace reactor {

/**
 * @class RedisCache
 * @brief Redis缓存管理器
 */
class RedisCache {
public:
    /**
     * @brief 获取单例实例
     */
    static RedisCache& GetInstance() {
        static RedisCache instance;
        return instance;
    }

    /**
     * @brief 构造函数
     */
    RedisCache();

    /**
     * @brief 析构函数
     */
    ~RedisCache();

    /**
     * @brief 初始化Redis缓存
     * @param host Redis主机地址
     * @param port Redis端口
     * @param password Redis密码
     * @param db Redis数据库编号
     * @param pool_size 连接池大小
     */
    void Initialize(const std::string& host, int port, const std::string& password = "", int db = 0, int pool_size = 10);

    /**
     * @brief 设置缓存
     * @param key 缓存键
     * @param value 缓存值
     * @param expire_seconds 过期时间（秒）
     * @return bool 是否设置成功
     */
    bool Set(const std::string& key, const std::string& value, int expire_seconds = 3600);

    /**
     * @brief 获取缓存
     * @param key 缓存键
     * @param value 输出参数：缓存值
     * @return bool 是否获取成功
     */
    bool Get(const std::string& key, std::string& value);

    /**
     * @brief 删除缓存
     * @param key 缓存键
     * @return bool 是否删除成功
     */
    bool Delete(const std::string& key);

    /**
     * @brief 检查缓存是否存在
     * @param key 缓存键
     * @return bool 是否存在
     */
    bool Exists(const std::string& key);

    /**
     * @brief 清空所有缓存
     * @return bool 是否清空成功
     */
    bool FlushAll();

private:
    // 线程安全锁
    std::mutex mutex_;
};

} // namespace reactor