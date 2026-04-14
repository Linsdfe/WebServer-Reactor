/**
 * @file auth.cpp
 * @brief 用户认证和会话管理模块实现
 * @details 实现用户登录验证、密码SHA256加密、会话ID生成与校验、
 *          会话过期管理、MySQL用户表操作等核心功能
 */

#include "auth/auth.h"
#include <random>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace reactor {

// 静态变量初始化
MYSQL* Auth::mysql_ = nullptr;
bool Auth::is_mysql_inited_ = false;
std::mutex Auth::mysql_mutex_;
std::string Auth::db_host_;
std::string Auth::db_user_;
std::string Auth::db_pwd_;
std::string Auth::db_db_;


// MySQL断线重连
bool Auth::ReconnectMySQL() {
    std::lock_guard<std::mutex> lock(mysql_mutex_);
    if (mysql_) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }

    mysql_ = mysql_init(nullptr);
    if (!mysql_) return false;

    if (!mysql_real_connect(mysql_, db_host_.c_str(), db_user_.c_str(), db_pwd_.c_str(), db_db_.c_str(), 0, nullptr, 0)) {
        mysql_close(mysql_);
        mysql_ = nullptr;
        return false;
    }
    return true;
}



/**
 * @brief 构造函数：初始化MySQL连接并创建基础用户环境
 * @param host MySQL主机地址
 * @param user MySQL用户名
 * @param password MySQL密码
 * @param database MySQL数据库名
 * 
 * 执行流程：
 * 1. 初始化MySQL句柄
 * 2. 连接数据库
 * 3. 创建用户表（不存在则自动创建）
 * 4. 添加默认管理员账户（admin/123456）
 */
Auth::Auth(const std::string& host, const std::string& user, const std::string& password, const std::string& database) {

      // 保存数据库配置
    db_host_ = host;
    db_user_ = user;
    db_pwd_ = password;
    db_db_ = database;

     // 全局只初始化一次MySQL连接
    if (!is_mysql_inited_ && !mysql_) {
        mysql_ = mysql_init(nullptr);
        if (!mysql_) {
            std::cerr << "MySQL初始化失败" << std::endl;
            return;
        }

        if (!mysql_real_connect(mysql_, host.c_str(), user.c_str(), password.c_str(), database.c_str(), 0, nullptr, 0)) {
            std::cerr << "MySQL连接失败: " << mysql_error(mysql_) << std::endl;
            mysql_close(mysql_);
            mysql_ = nullptr;
            return;
        }
        is_mysql_inited_ = true;
        CreateUserTable();
        AddUser("admin", "123456");
    }
}

/**
 * @brief 析构函数：释放数据库资源
 *
 * 功能：安全关闭MySQL连接，避免资源泄漏
 */
Auth::~Auth() {
    std::lock_guard<std::mutex> lock(mysql_mutex_);
     if (is_mysql_inited_ && mysql_) {
        mysql_close(mysql_);
        mysql_ = nullptr;
        is_mysql_inited_ = false;
    }
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
    // 执行SHA256哈希计算
    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.length(), hash);
    
    // 转为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

/**
 * @brief 验证用户名密码是否正确
 * @param username 用户名
 * @param password 明文密码
 * @return 验证成功返回true，失败返回false
 *
 * 流程：
 * 1. 检查MySQL连接状态
 * 2. 对传入密码做哈希
 * 3. 查询users表匹配用户名和哈希密码
 * 4. 返回是否存在匹配记录
 */
bool Auth::ValidateUser(const std::string& username, const std::string& password) {
     std::lock_guard<std::mutex> lock(mysql_mutex_);
    if (!mysql_) {
        if (!ReconnectMySQL()) {
            std::cerr << "[ERROR] MySQL重连失败" << std::endl;
            return false;
        }
    }

    //std::cout << "[INFO] 验证用户：" << username << std::endl;
    if (!mysql_) {
        //std::cout << "[ERROR] MySQL连接未初始化" << std::endl;
        return false;
    }
    
    // 密码哈希处理
    std::string hashed_password = HashPassword(password);
    //std::cout << "[INFO] 密码加密：" << hashed_password << std::endl;
    
    // 拼接查询SQL
    std::string query = "SELECT id FROM users WHERE username = '" + username + "' AND password = '" + hashed_password + "'";
    //std::cout << "[INFO] 执行查询：" << query << std::endl;
    
    // 执行查询
    if (mysql_query(mysql_, query.c_str())) {
        std::cerr << "[ERROR] MySQL查询失败：" << mysql_error(mysql_) << std::endl;
        return false;
    }
    
    // 获取结果集
    MYSQL_RES* result = mysql_store_result(mysql_);
    if (!result) {
        std::cerr << "[ERROR] 获取查询结果失败：" << mysql_error(mysql_) << std::endl;
        return false;
    }
    
    // 存在记录则验证成功
    bool success = mysql_num_rows(result) > 0;
    //std::cout << "[INFO] 验证结果：" << (success ? "成功" : "失败") << std::endl;
    
    // 释放结果集内存
    mysql_free_result(result);
    
    return success;
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
    // 生成32位随机字符串作为会话ID
    std::string session_id = GenerateRandomString(32);
    
    // 保存会话与用户对应关系
    sessions_[session_id] = username;
    
    // 设置会话过期时间（当前时间+1小时）
    auto now = std::chrono::steady_clock::now();
    session_expiry_[session_id] = now + std::chrono::seconds(SESSION_EXPIRY_SECONDS);
    
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
    std::lock_guard<std::mutex> lock(mysql_mutex_);
    if (!mysql_) {
        return;
    }
    
    std::string query = "CREATE TABLE IF NOT EXISTS users (" 
                      "id INT AUTO_INCREMENT PRIMARY KEY, " 
                      "username VARCHAR(50) NOT NULL UNIQUE, " 
                      "password VARCHAR(64) NOT NULL, " 
                      "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP" 
                      ");";
    
    if (mysql_query(mysql_, query.c_str())) {
        std::cerr << "创建用户表失败: " << mysql_error(mysql_) << std::endl;
    }
}

/**
 * @brief 添加新用户（已存在则忽略）
 * @param username 用户名
 * @param password 明文密码
 * @return 添加成功返回true，用户已存在或失败返回false
 */
bool Auth::AddUser(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(mysql_mutex_);
    //std::cout << "[INFO] 添加用户：" << username << std::endl;
    if (!mysql_) {
        //std::cout << "[ERROR] MySQL连接未初始化" << std::endl;
        return false;
    }
    
    // 密码哈希加密
    std::string hashed_password = HashPassword(password);
    //std::cout << "[INFO] 密码加密：" << hashed_password << std::endl;
    
    // INSERT IGNORE：用户已存在则不插入且不报错
    std::string query = "INSERT IGNORE INTO users (username, password) VALUES ('" + username + "', '" + hashed_password + "')";
    //std::cout << "[INFO] 执行查询：" << query << std::endl;
    
    if (mysql_query(mysql_, query.c_str())) {
        std::cerr << "[ERROR] 添加用户失败：" << mysql_error(mysql_) << std::endl;
        return false;
    }
    
    // 受影响行数>0表示新插入成功
    int affected_rows = mysql_affected_rows(mysql_);
    //std::cout << "[INFO] 受影响行数：" << affected_rows << std::endl;
    
    if (affected_rows == 0) {
        //std::cout << "[INFO] 用户已存在：" << username << std::endl;
        return false;
    }
    
    //std::cout << "[INFO] 用户添加成功：" << username << std::endl;
    return true;
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