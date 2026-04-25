/**
 * @file cachemanager.h
 * @brief 静态资源内存缓存管理器
 *
 * 核心功能：
 * 1. 管理静态文件的内存缓存
 * 2. 提供线程安全的缓存访问
 * 3. 支持缓存大小限制和LRU淘汰策略
 * 4. 自动加载和更新缓存
 */

#pragma once

#include <string>
#include <unordered_map>
#include <list>
#include <memory>
#include <shared_mutex>

namespace reactor {

/**
 * @brief 缓存项结构
 */
struct CacheItem {
    std::shared_ptr<std::string> content; // 文件内容（使用shared_ptr实现零拷贝）
    size_t size;              // 文件大小
};

/**
 * @brief 静态资源内存缓存管理器
 */
class CacheManager {
public:
    /**
     * @brief 构造函数
     * @param max_size 缓存最大大小（字节）
     */
    explicit CacheManager(size_t max_size = 1024 * 1024 * 100); // 默认100MB

    /**
     * @brief 析构函数
     */
    ~CacheManager();

    /**
     * @brief 获取缓存的文件内容
     * @param file_path 文件路径
     * @param content 输出参数：文件内容
     * @return bool 是否命中缓存
     */
    bool GetCache(const std::string& file_path, std::string& content);

    /**
     * @brief 添加或更新缓存
     * @param file_path 文件路径
     * @param content 文件内容
     */
    void SetCache(const std::string& file_path, const std::string& content);

    /**
     * @brief 清除所有缓存
     */
    void ClearCache();

    /**
     * @brief 获取缓存统计信息
     * @param item_count 输出参数：缓存项数量
     * @param current_size 输出参数：当前缓存大小
     */
    void GetStats(size_t& item_count, size_t& current_size);

private:
    /**
     * @brief 淘汰最久未使用的缓存项
     */
    void EvictLRU();

    size_t max_size_;          // 缓存最大大小
    size_t current_size_;      // 当前缓存大小

    // 使用双向链表跟踪访问顺序，头部是最近访问的，尾部是最久未访问的
    std::list<std::string> lru_list_;
    // 缓存映射，值为pair<CacheItem, list iterator>
    std::unordered_map<std::string, std::pair<CacheItem, std::list<std::string>::iterator>> cache_;

    std::shared_mutex rwlock_;
};

} // namespace reactor