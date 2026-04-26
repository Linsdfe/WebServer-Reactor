/**
 * @file auth.cpp
 * @brief 用户认证和会话管理模块实现
 * @details 实现用户登录验证、密码SHA256加密、会话ID生成与校验、
 *          会话过期管理、MySQL用户表操作等核心功能
 */

#include "auth/auth.h"
#include "monitor/metrics_collector.h"
#include <random>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstring>

namespace reactor {

/**
 * @brief 构造函数：初始化MySQL连接池并创建基础用户环境
 * @param host MySQL主机地址
 * @param user MySQL用户名
 * @param password MySQL密码
 * @param database MySQL数据库名
 * 
 * 执行流程：
 * 1. 保存数据库配置
 * 2. 初始化MySQL连接池
 * 3. 创建用户表（不存在则自动创建）
 * 4. 添加默认管理员账户（admin/123456）
 */
Auth::Auth() {
    // 创建用户表
    CreateUserTable();
    // 添加默认管理员账户
    AddUser("admin", "123456");
}

/**
 * @brief 析构函数：清理内存中的会话数据
 *
 * 功能：清理会话数据，连接池由单例管理，不需要在这里关闭
 */
Auth::~Auth() {
    // 清理会话数据
    sessions_.clear();
    session_expiry_.clear();
}

/**
 * @brief 使用SHA256算法对密码进行哈希加密
 * @param password 明文密码
 * @return 加密后的64位十六进制哈希字符串
 *
 * 安全说明：明文密码永不存储，仅存储哈希值
 */
std::string Auth::HashPassword(const std::string& password) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.length(), hash);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

/**
 * @brief 从数据库查询用户信息
 * @param username 用户名
 * @param password_hash 输出参数，存储密码哈希
 * @return 查询结果：true=成功，false=失败
 */
bool Auth::QueryUserFromDB(const std::string& username, std::string& password_hash) {
    // 先从Redis缓存查询
    if (RedisCache::GetInstance().Get("user:" + username, password_hash)) {
        MetricsCollector::Instance().IncRedisCacheHits();  // Redis缓存命中
        return true;
    }
    MetricsCollector::Instance().IncRedisCacheMisses();  // Redis缓存未命中
    
    // 从连接池获取从库连接（读操作）
    MySQLConnection* conn = MySQLConnectionPool::GetInstance().GetSlaveConnection(3000);
    if (!conn) {
        std::cerr << "[ERROR] 获取数据库连接失败" << std::endl;
        return false;
    }

    MYSQL* mysql = conn->GetRawConnection();
    if (!mysql) {
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) {
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    const char* query = "SELECT password FROM users WHERE username = ?";
    if (mysql_stmt_prepare(stmt, query, static_cast<unsigned long>(strlen(query)))) {
        std::cerr << "[ERROR] mysql_stmt_prepare failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    MYSQL_BIND param[1];
    memset(param, 0, sizeof(param));
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = const_cast<char*>(username.c_str());
    param[0].buffer_length = static_cast<unsigned long>(username.length());

    if (mysql_stmt_bind_param(stmt, param)) {
        std::cerr << "[ERROR] mysql_stmt_bind_param failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    if (mysql_stmt_execute(stmt)) {
        std::cerr << "[ERROR] mysql_stmt_execute failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    char password_buf[256] = {0};
    unsigned long password_len = 0;
    MYSQL_BIND result[1];
    memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_STRING;
    result[0].buffer = password_buf;
    result[0].buffer_length = sizeof(password_buf);
    result[0].length = &password_len;

    if (mysql_stmt_bind_result(stmt, result)) {
        std::cerr << "[ERROR] mysql_stmt_bind_result failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
        return false;
    }

    bool success = false;
    if (mysql_stmt_fetch(stmt) == 0) {
        password_hash = std::string(password_buf, password_len);
        success = true;
        RedisCache::GetInstance().Set("user:" + username, password_hash, 3600);
        MetricsCollector::Instance().IncRedisCacheUpdates();
    }

    mysql_stmt_close(stmt);
    MySQLConnectionPool::GetInstance().ReturnSlaveConnection(conn);
    
    return success;
}

/**
 * @brief 验证用户名密码是否正确
 * @param username 用户名
 * @param password 明文密码
 * @return 验证成功返回true，失败返回false
 *
 * 流程：
 * 1. 对传入密码做哈希
 * 2. 查询数据库匹配用户名和哈希密码
 * 3. 返回是否存在匹配记录
 */
bool Auth::ValidateUser(const std::string& username, const std::string& password) {
    // 密码哈希处理
    std::string hashed_password = HashPassword(password);
    
    // 从数据库查询用户信息
    std::string stored_hash;
    if (QueryUserFromDB(username, stored_hash)) {
        return stored_hash == hashed_password;
    }
    
    return false;
}

/**
 * @brief 为登录成功的用户生成会话ID
 * @param username 已验证的用户名
 * @return 32位随机字符串会话ID
 *
 * 功能：
 * 1. 生成安全随机串作为session_id
 * 2. 建立session_id → username映射
 * 3. 设置会话1小时过期
 */
std::string Auth::GenerateSessionId(const std::string& username) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    
    // 生成32位随机字符串作为会话ID
    std::string session_id = GenerateRandomString(32);
    
    // 保存会话与用户对应关系
    sessions_[session_id] = username;
    
    // 设置会话过期时间（当前时间+1小时）
    auto now = std::chrono::steady_clock::now();
    session_expiry_[session_id] = now + std::chrono::seconds(SESSION_EXPIRY_SECONDS);
    
    // 将会话信息缓存到Redis
    RedisCache::GetInstance().Set("session:" + session_id, username, SESSION_EXPIRY_SECONDS);
    MetricsCollector::Instance().IncRedisCacheUpdates();  // Redis缓存更新

    return session_id;
}

/**
 * @brief 校验客户端会话ID是否有效
 * @param session_id 客户端传入的会话ID
 * @return 有效返回true，无效/过期返回false
 *
 * 逻辑：
 * 1. 先清理所有过期会话
 * 2. 检查session_id是否存在
 * 3. 检查是否在有效期内
 * 4. 验证通过则自动续期1小时
 */
bool Auth::ValidateSession(const std::string& session_id) {
    // 先从Redis缓存检查会话
    std::string username;
    if (RedisCache::GetInstance().Get("session:" + session_id, username)) {
        MetricsCollector::Instance().IncRedisCacheHits();  // Redis缓存命中
        // 会话有效，更新过期时间
        RedisCache::GetInstance().Set("session:" + session_id, username, SESSION_EXPIRY_SECONDS);
        MetricsCollector::Instance().IncRedisCacheUpdates();  // Redis缓存更新
        return true;
    }
    MetricsCollector::Instance().IncRedisCacheMisses();  // Redis缓存未命中
    
    std::lock_guard<std::mutex> lock(session_mutex_);
    
    // 每次验证前清理过期会话
    CleanExpiredSessions();
    
    // 查找会话是否存在
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        // 检查会话是否过期
        auto now = std::chrono::steady_clock::now();
        if (session_expiry_[session_id] > now) {
            // 会话有效，自动续期
            session_expiry_[session_id] = now + std::chrono::seconds(SESSION_EXPIRY_SECONDS);
            // 将会话信息缓存到Redis
            RedisCache::GetInstance().Set("session:" + session_id, it->second, SESSION_EXPIRY_SECONDS);
            MetricsCollector::Instance().IncRedisCacheUpdates();  // Redis缓存更新
            return true;
        }
    }
    return false;
}

/**
 * @brief 清理所有已过期的会话
 *
 * 流程：
 * 1. 遍历会话过期表，收集所有已过期session_id
 * 2. 从sessions和session_expiry_中批量删除
 * 3. 释放内存，避免会话堆积
 */
void Auth::CleanExpiredSessions() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_sessions;

    // 收集所有过期会话ID
    for (const auto& pair : session_expiry_) {
        if (pair.second <= now) {
            expired_sessions.push_back(pair.first);
        }
    }

    // 删除过期会话
    for (const auto& session_id : expired_sessions) {
        sessions_.erase(session_id);
        session_expiry_.erase(session_id);
        // 从Redis中删除过期会话
        RedisCache::GetInstance().Delete("session:" + session_id);
        MetricsCollector::Instance().IncRedisCacheExpirations();  // Redis缓存过期
    }
}

/**
 * @brief 创建用户表（不存在则创建）
 *
 * 表结构：
 * id: 自增主键
 * username: 用户名（唯一）
 * password: 密码哈希（64位）
 * created_at: 创建时间戳
 */
void Auth::CreateUserTable() {
    // 从连接池获取主库连接（写操作）
    MySQLConnection* conn = MySQLConnectionPool::GetInstance().GetMasterConnection(3000);
    if (!conn) {
        std::cerr << "[ERROR] 获取数据库连接失败" << std::endl;
        return;
    }

    MYSQL* mysql = conn->GetRawConnection();
    if (!mysql) {
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return;
    }
    
    std::string query = "CREATE TABLE IF NOT EXISTS users (" 
                      "id INT AUTO_INCREMENT PRIMARY KEY, " 
                      "username VARCHAR(50) NOT NULL UNIQUE, " 
                      "password VARCHAR(64) NOT NULL, " 
                      "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP" 
                      ");";
    
    if (mysql_query(mysql, query.c_str())) {
        std::cerr << "创建用户表失败: " << mysql_error(mysql) << std::endl;
    }
    
    // 归还连接
    MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
}

/**
 * @brief 添加新用户（已存在则忽略）
 * @param username 用户名
 * @param password 明文密码
 * @return 添加成功返回true，用户已存在或失败返回false
 */
bool Auth::AddUser(const std::string& username, const std::string& password) {
    // 先检查Redis缓存，快速判断用户是否已存在
    std::string existing_password;
    if (RedisCache::GetInstance().Get("user:" + username, existing_password)) {
        // 用户已存在于缓存，直接返回失败
        return false;
    }
    
    // 密码哈希加密
    std::string hashed_password = HashPassword(password);
    
    // 从连接池获取主库连接（写操作）
    MySQLConnection* conn = MySQLConnectionPool::GetInstance().GetMasterConnection(3000);
    if (!conn) {
        std::cerr << "[ERROR] 获取数据库连接失败" << std::endl;
        return false;
    }

    MYSQL* mysql = conn->GetRawConnection();
    if (!mysql) {
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return false;
    }
    
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) {
        std::cerr << "[ERROR] mysql_stmt_init failed" << std::endl;
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return false;
    }

    const char* query = "INSERT IGNORE INTO users (username, password) VALUES (?, ?)";
    if (mysql_stmt_prepare(stmt, query, static_cast<unsigned long>(strlen(query)))) {
        std::cerr << "[ERROR] mysql_stmt_prepare failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return false;
    }

    MYSQL_BIND params[2];
    memset(params, 0, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(username.c_str());
    params[0].buffer_length = static_cast<unsigned long>(username.length());
    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(hashed_password.c_str());
    params[1].buffer_length = static_cast<unsigned long>(hashed_password.length());

    if (mysql_stmt_bind_param(stmt, params)) {
        std::cerr << "[ERROR] mysql_stmt_bind_param failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return false;
    }

    if (mysql_stmt_execute(stmt)) {
        std::cerr << "[ERROR] 添加用户失败：" << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
        return false;
    }

    int affected_rows = static_cast<int>(mysql_stmt_affected_rows(stmt));

    mysql_stmt_close(stmt);
    MySQLConnectionPool::GetInstance().ReturnMasterConnection(conn);
    
    if (affected_rows > 0) {
        RedisCache::GetInstance().Delete("user:" + username);
        RedisCache::GetInstance().Set("user:" + username, hashed_password, 3600);
        MetricsCollector::Instance().IncRedisCacheUpdates();
    }

    return affected_rows > 0;
}

/**
 * @brief 生成指定长度的安全随机字符串
 * @param length 目标字符串长度
 * @return 随机字符串（包含数字、大小写字母）
 *
 * 用于：会话ID、加密盐值等安全场景
 */
std::string Auth::GenerateRandomString(int length) {
    const std::string chars = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> distribution(0, chars.size() - 1);
    
    std::string result;
    for (int i = 0; i < length; ++i) {
        result += chars[distribution(generator)];
    }
    return result;
}

} // namespace reactor