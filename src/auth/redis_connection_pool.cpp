#include "auth/redis_connection_pool.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>

namespace reactor {

RedisConnection::RedisConnection(redisContext* conn)
    : conn_(conn) {
}

RedisConnection::~RedisConnection() {
    Close();
}

redisContext* RedisConnection::GetRawConnection() {
    return conn_;
}

bool RedisConnection::IsValid() {
    return conn_ != nullptr && conn_->err == 0;
}

void RedisConnection::Close() {
    if (conn_) {
        redisFree(conn_);
        conn_ = nullptr;
    }
}

RedisConnectionPool::RedisConnectionPool()
    : is_initialized_(false),
      active_count_(0) {
}

RedisConnectionPool::~RedisConnectionPool() {
    Close();
}

void RedisConnectionPool::Initialize(const std::string& host, int port, const std::string& password, int db, int pool_size, int max_idle_time) {
    if (is_initialized_) {
        return;
    }

    host_ = host;
    port_ = port;
    password_ = password;
    db_ = db;
    pool_size_ = pool_size;
    max_idle_time_ = max_idle_time;

    // 预创建连接
    std::lock_guard<std::mutex> lock(mutex_);
    int created = 0;
    for (int i = 0; i < pool_size_; ++i) {
        RedisConnection* conn = CreateConnection();
        if (conn && CheckConnection(conn)) {
            connections_.push(conn);
            created++;
        } else if (conn) {
            delete conn;
        }
    }
    std::cout << "[Redis] Created " << created << " connections out of " << pool_size_ << std::endl;

    is_initialized_ = true;
}

RedisConnection* RedisConnectionPool::GetConnection(int timeout_ms) {
    if (!is_initialized_) {
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto timeout = std::chrono::milliseconds(timeout_ms);

    while (connections_.empty()) {
        if (active_count_ < pool_size_) {
            // 创建新连接
            RedisConnection* conn = CreateConnection();
            if (conn) {
                active_count_++;
                return conn;
            }
        }

        // 等待连接可用
        if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
            return nullptr;
        }
    }

    RedisConnection* conn = connections_.front();
    connections_.pop();

    // 使用redisContext的err字段检查连接是否有效，而不频繁发送PING命令
    if (!conn->IsValid()) {
        delete conn;
        conn = CreateConnection();
    }

    active_count_++;
    return conn;
}

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

void RedisConnectionPool::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) {
        RedisConnection* conn = connections_.front();
        connections_.pop();
        delete conn;
    }
    is_initialized_ = false;
}

int RedisConnectionPool::GetIdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

RedisConnection* RedisConnectionPool::CreateConnection() {
    redisContext* ctx = redisConnect(host_.c_str(), port_);
    if (!ctx) {
        std::cerr << "[Redis] Failed to create context" << std::endl;
        return nullptr;
    }
    if (ctx->err) {
        std::cerr << "[Redis] Connection error: " << ctx->errstr << std::endl;
        redisFree(ctx);
        return nullptr;
    }

    // 认证
    if (!password_.empty()) {
        redisReply* reply = (redisReply*)redisCommand(ctx, "AUTH %s", password_.c_str());
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    if (db_ != 0) {
        redisReply* reply = (redisReply*)redisCommand(ctx, "SELECT %d", db_);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    return new RedisConnection(ctx);
}

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

void RedisConnectionPool::CleanExpiredConnections() {
}

} // namespace reactor