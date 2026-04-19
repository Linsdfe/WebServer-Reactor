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
    pthread_rwlock_init(&rwlock_, nullptr);
}

/**
 * @brief 析构函数
 */
CacheManager::~CacheManager() {
    ClearCache();
    pthread_rwlock_destroy(&rwlock_);
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
    // 获取读锁，不更新LRU，避免写锁开销
    pthread_rwlock_rdlock(&rwlock_);

    auto it = cache_.find(file_path);
    if (it == cache_.end()) {
        pthread_rwlock_unlock(&rwlock_);
        return false; // 缓存未命中
    }

    // 直接拷贝内容（shared_ptr保证数据共享）
    content = *it->second.first.content;
    pthread_rwlock_unlock(&rwlock_);
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
    pthread_rwlock_wrlock(&rwlock_);

    size_t content_size = content.size();

    // 如果内容大小超过缓存最大容量，不缓存
    if (content_size > max_size_) {
        pthread_rwlock_unlock(&rwlock_);
        return;
    }

    // 如果已存在缓存，先移除旧的
    auto it = cache_.find(file_path);
    if (it != cache_.end()) {
        current_size_ -= it->second.first.size;
        lru_list_.erase(it->second.second);
        cache_.erase(it);
    }

    // 确保有足够空间，淘汰最久未使用的项
    while (current_size_ + content_size > max_size_) {
        EvictLRU();
    }

    // 添加新缓存，使用shared_ptr存储内容实现零拷贝
    CacheItem item;
    item.content = std::make_shared<std::string>(content);
    item.size = content_size;

    // 将文件路径添加到链表头部（最近使用）
    lru_list_.push_front(file_path);
    auto list_it = lru_list_.begin();

    cache_[file_path] = std::make_pair(item, list_it);
    current_size_ += content_size;

    pthread_rwlock_unlock(&rwlock_);
}

/**
 * @brief 清除所有缓存
 */
void CacheManager::ClearCache() {
    pthread_rwlock_wrlock(&rwlock_);
    cache_.clear();
    lru_list_.clear();
    current_size_ = 0;
    pthread_rwlock_unlock(&rwlock_);
}

/**
 * @brief 获取缓存统计信息
 * @param item_count 输出参数：缓存项数量
 * @param current_size 输出参数：当前缓存大小
 */
void CacheManager::GetStats(size_t& item_count, size_t& current_size) {
    pthread_rwlock_rdlock(&rwlock_);
    item_count = cache_.size();
    current_size = current_size_;
    pthread_rwlock_unlock(&rwlock_);
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