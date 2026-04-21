#include "server/redis_cache.h"
#include <hiredis/hiredis.h>
#include <cstring>
namespace reactor {

RedisCache::RedisCache() {
}

RedisCache::~RedisCache() {
}

void RedisCache::Initialize(const std::string& host, int port, const std::string& password, int db, int pool_size) {
    RedisConnectionPool::GetInstance().Initialize(host, port, password, db, pool_size);
}

bool RedisCache::Set(const std::string& key, const std::string& value, int expire_seconds) {
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
    if (expire_seconds > 0) {
        redisReply* reply = (redisReply*)redisCommand(ctx, "SETEX %s %d %s", key.c_str(), expire_seconds, value.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
            result = true;
        }
        freeReplyObject(reply);
    } else {
        redisReply* reply = (redisReply*)redisCommand(ctx, "SET %s %s", key.c_str(), value.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
            result = true;
        }
        freeReplyObject(reply);
    }

    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

bool RedisCache::Get(const std::string& key, std::string& value) {
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
    redisReply* reply = (redisReply*)redisCommand(ctx, "GET %s", key.c_str());
    if (reply && reply->type == REDIS_REPLY_STRING) {
        value = reply->str;
        result = true;
    }
    freeReplyObject(reply);

    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

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

bool RedisCache::Exists(const std::string& key) {
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
    redisReply* reply = (redisReply*)redisCommand(ctx, "EXISTS %s", key.c_str());
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) {
        result = true;
    }
    freeReplyObject(reply);

    RedisConnectionPool::GetInstance().ReturnConnection(conn);
    return result;
}

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

} // namespace reactor