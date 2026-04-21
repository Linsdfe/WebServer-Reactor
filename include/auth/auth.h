#pragma once

/**
 * @file auth.h
 * @brief 用户认证和会话管理模块
 * 
 * 核心功能：
 * 1. 用户认证（用户名密码验证）
 * 2. 会话管理（生成和验证会话ID）
 * 3. 登录状态维护
 * 
 * 依赖说明：
 * 1. MySQL C API：用于用户信息的持久化存储
 * 2. OpenSSL SHA：用于密码哈希加密
 * 3. C++11 及以上：使用 unordered_map、chrono 等标准库
 * 4. MySQL 连接池：优化数据库连接管理
 */

#include <string>
#include <unordered_map>
#include <chrono>
#include <mysql/mysql.h>
#include <openssl/sha.h>
#include <mutex>  // 线程锁
#include "auth/mysql_connection_pool.h"
#include "server/redis_cache.h"

namespace reactor {

/**
 * @class Auth
 * @brief 用户认证与会话管理核心类
 * 
 * 负责用户登录验证、密码加密存储、会话ID生成与校验、过期会话清理
 * 采用内存+数据库结合的方式：用户信息持久化到MySQL，会话信息存储在内存
 */
class Auth {
public:
    /**
     * @brief 构造函数
     */
    Auth();
    
    /**
     * @brief 析构函数
     * 
     * 功能：清理内存中的会话数据
     */
    ~Auth();
    
    /**
     * @brief 验证用户登录信息
     * @param username 客户端传入的用户名
     * @param password 客户端传入的明文密码
     * @return bool 验证结果：true=验证成功，false=验证失败
     * 
     * 逻辑：
     * 1. 对传入密码做哈希处理
     * 2. 查询数据库比对用户名和哈希密码
     * 3. 返回比对结果
     */
    bool ValidateUser(const std::string& username, const std::string& password);
    
    /**
     * @brief 为已登录用户生成唯一会话ID
     * @param username 已验证通过的用户名
     * @return std::string 生成的会话ID字符串
     * 
     * 逻辑：
     * 1. 生成随机字符串 + 用户名信息做唯一标识
     * 2. 存储会话与用户的映射关系
     * 3. 设置会话过期时间
     */
    std::string GenerateSessionId(const std::string& username);
    
    /**
     * @brief 验证会话ID是否有效
     * @param session_id 客户端携带的会话ID
     * @return bool 验证结果：true=有效，false=无效/过期
     * 
     * 逻辑：
     * 1. 检查会话ID是否存在
     * 2. 检查会话是否过期
     * 3. 同时清理过期会话
     */
    bool ValidateSession(const std::string& session_id);
    
    /**
     * @brief 主动清理所有过期的会话
     * 
     * 功能：遍历内存会话表，删除已过期的会话记录
     * 可定时调用，释放内存资源
     */
    void CleanExpiredSessions();
    
    /**
     * @brief 初始化数据库：创建用户表
     * 
     * 功能：若数据库中不存在用户表，则自动创建
     * 表结构包含用户名、密码哈希字段
     */
    void CreateUserTable();
    
    /**
     * @brief 注册新用户
     * @param username 新用户名
     * @param password 明文密码
     * @return bool 添加结果：true=成功，false=失败
     * 
     * 逻辑：
     * 1. 密码哈希加密
     * 2. 插入数据库用户表
     * 3. 返回插入结果
     */
    bool AddUser(const std::string& username, const std::string& password);
    
private:
    
    /**
     * @brief 从数据库查询用户信息
     * @param username 用户名
     * @param password_hash 输出参数，存储密码哈希
     * @return bool 查询结果：true=成功，false=失败
     */
    bool QueryUserFromDB(const std::string& username, std::string& password_hash);
    
    /**
     * @brief 生成指定长度的安全随机字符串
     * @param length 目标字符串长度
     * @return std::string 随机字符串
     * 
     * 用途：用于生成会话ID、盐值等
     */
    std::string GenerateRandomString(int length);
    
    /**
     * @brief 使用 SHA256 对密码进行哈希加密
     * @param password 用户明文密码
     * @return std::string 十六进制格式的哈希字符串
     * 
     * 安全说明：明文密码永不存储，仅保存哈希值
     */
    std::string HashPassword(const std::string& password);
    
private:
    // 数据库配置
    std::string db_host_;
    std::string db_user_;
    std::string db_pwd_;
    std::string db_db_;

    // 内存会话表：key=会话ID，value=用户名
    std::unordered_map<std::string, std::string> sessions_;
    
    // 会话过期时间表：key=会话ID，value=会话过期时间点
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> session_expiry_;
    
    // 会话默认过期时间：3600 秒（1 小时）
    const int SESSION_EXPIRY_SECONDS = 3600;
    
    // 线程安全锁
    std::mutex session_mutex_;
};

} // namespace reactor