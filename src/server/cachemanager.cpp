/**
 * @file cachemanager.cpp
 * @brief 静态资源内存缓存管理器实现
 */

#include "server/cachemanager.h"
#include <sys/stat.h>
#include <iostream>

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
 */
bool CacheManager::GetCache(const std::string& file_path, std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_.find(file_path);
    if (it != cache_.end()) {
        // 缓存命中，将文件路径移到链表头部（标记为最近使用）
        lru_list_.erase(it->second.second);
        lru_list_.push_front(file_path);
        it->second.second = lru_list_.begin();
        
        // 更新访问时间
        it->second.first.last_access = std::chrono::steady_clock::now();
        content = it->second.first.content;
        return true;
    }
    
    return false;
}

/**
 * @brief 添加或更新缓存
 * @param file_path 文件路径
 * @param content 文件内容
 */
void CacheManager::SetCache(const std::string& file_path, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t content_size = content.size();
    
    // 如果内容大小超过缓存最大容量，不缓存
    if (content_size > max_size_) {
        return;
    }
    
    // 如果已存在缓存，先移除旧的
    auto it = cache_.find(file_path);
    if (it != cache_.end()) {
        current_size_ -= it->second.first.size;
        lru_list_.erase(it->second.second);
        cache_.erase(it);
    }
    
    // 确保有足够空间
    while (current_size_ + content_size > max_size_) {
        EvictLRU();
    }
    
    // 获取文件修改时间
    struct stat file_stat;
    time_t mtime = 0;
    if (stat(file_path.c_str(), &file_stat) == 0) {
        mtime = file_stat.st_mtime;
    }
    
    // 添加新缓存
    CacheItem item;
    item.content = content;
    item.size = content_size;
    item.last_access = std::chrono::steady_clock::now();
    item.file_mtime = mtime;
    
    // 将文件路径添加到链表头部
    lru_list_.push_front(file_path);
    auto list_it = lru_list_.begin();
    
    cache_[file_path] = std::make_pair(item, list_it);
    current_size_ += content_size;
}

/**
 * @brief 清除所有缓存
 */
void CacheManager::ClearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    item_count = cache_.size();
    current_size = current_size_;
}

/**
 * @brief 淘汰最久未使用的缓存项
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