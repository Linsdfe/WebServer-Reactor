/**
 * @file cachemanager.cpp
 * @brief 静态资源内存缓存管理器实现
 */

#include "server/cachemanager.h"

namespace reactor {

/**
 * @brief 构造函数
 * @param max_size 缓存最大大小（字节）
 */
CacheManager::CacheManager(size_t max_size)
    : max_size_(max_size), current_size_(0) {
}

/**
 * @brief 析构函数
 */
CacheManager::~CacheManager() {
    ClearCache();
}

/**
 * @brief 获取缓存的文件内容
 * @param file_path 文件路径
 * @param content 输出参数：文件内容
 * @return bool 是否命中缓存
 *
 * 【优化策略】读操作使用读锁，不更新LRU，确保高性能
 */
bool CacheManager::GetCache(const std::string& file_path, std::string& content) {
    std::shared_lock<std::shared_mutex> lock(rwlock_);

    auto it = cache_.find(file_path);
    if (it == cache_.end()) {
        return false;
    }

    content = *it->second.first.content;
    return true;
}

/**
 * @brief 添加或更新缓存
 * @param file_path 文件路径
 * @param content 文件内容
 *
 * 【LRU维护】写操作时更新LRU链表，保证缓存有效性
 */
void CacheManager::SetCache(const std::string& file_path, const std::string& content) {
    std::unique_lock<std::shared_mutex> lock(rwlock_);

    size_t content_size = content.size();

    if (content_size > max_size_) {
        return;
    }

    auto it = cache_.find(file_path);
    if (it != cache_.end()) {
        current_size_ -= it->second.first.size;
        lru_list_.erase(it->second.second);
        cache_.erase(it);
    }

    while (current_size_ + content_size > max_size_) {
        EvictLRU();
    }

    CacheItem item;
    item.content = std::make_shared<std::string>(content);
    item.size = content_size;

    lru_list_.push_front(file_path);
    auto list_it = lru_list_.begin();

    cache_[file_path] = std::make_pair(item, list_it);
    current_size_ += content_size;
}

/**
 * @brief 清除所有缓存
 */
void CacheManager::ClearCache() {
    std::unique_lock<std::shared_mutex> lock(rwlock_);
    cache_.clear();
    lru_list_.clear();
    current_size_ = 0;
}

/**
 * @brief 获取缓存统计信息
 * @param item_count 输出参数：缓存项数量
 * @param current_size 输出参数：当前缓存大小
 */
void CacheManager::GetStats(size_t& item_count, size_t& current_size) {
    std::shared_lock<std::shared_mutex> lock(rwlock_);
    item_count = cache_.size();
    current_size = current_size_;
}

/**
 * @brief 淘汰最久未使用的缓存项
 * 【LRU核心】从链表尾部移除最久未使用的项
 */
void CacheManager::EvictLRU() {
    if (cache_.empty() || lru_list_.empty()) {
        return;
    }

    // 移除链表尾部的元素（最久未使用的）
    std::string lru_key = lru_list_.back();
    lru_list_.pop_back();

    auto it = cache_.find(lru_key);
    if (it != cache_.end()) {
        current_size_ -= it->second.first.size;
        cache_.erase(it);
    }
}

} // namespace reactor