#pragma once

/**
 * @file tcpconnection.h
 * @brief TcpConnection类：管理单个TCP连接的生命周期和IO
 * 
 * TcpConnection核心职责：
 * 1. 管理连接fd的生命周期（建立/销毁）
 * 2. 处理IO事件（读/写/关闭/错误）
 * 3. 解析HTTP请求（调用HttpRequest）
 * 4. 构建HTTP响应（调用HttpResponse）
 * 5. 处理长连接/短连接
 */
#include "net/eventloop.h"
#include "net/channel.h"
#include "http/httprequest.h"
#include "http/httpresponse.h"
#include "auth/auth.h"
#include "server/cachemanager.h"
#include <memory>
#include <string>

namespace reactor {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    /**
     * @brief 连接关闭回调类型
     */
    using CloseCallback = std::function<void(int)>;

    /**
     * @brief 构造函数
     * @param loop 所属的EventLoop
     * @param fd 连接的socket fd
     * @param src_dir 静态资源目录
     * @param mysql_host MySQL主机地址
     * @param mysql_user MySQL用户名
     * @param mysql_password MySQL密码
     * @param mysql_database MySQL数据库名
     * @param cache_manager 静态资源缓存管理器
     */
    TcpConnection(EventLoop* loop, int fd, const std::string& src_dir, 
                 const std::string& mysql_host, const std::string& mysql_user, 
                 const std::string& mysql_password, const std::string& mysql_database,
                 const std::shared_ptr<CacheManager>& cache_manager);
    
    /**
     * @brief 析构函数：关闭连接fd
     */
    ~TcpConnection();

    /**
     * @brief 设置连接关闭回调（通知Server清理）
     * @param cb 回调函数
     */
    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }
    
    /**
     * @brief 连接建立初始化（注册fd到Epoller，开启读事件）
     */
    void ConnectEstablished();
    
    /**
     * @brief 连接销毁（注销Epoller事件，清理资源）
     */
    void ConnectDestroyed();
    
    /**
     * @brief 获取所属的EventLoop（【修复】添加Getter保证线程安全）
     * @return EventLoop* 所属的从Reactor
     */
    EventLoop* GetLoop() const { return loop_; }

private:
    /**
     * @brief 处理读事件（读取TCP数据，解析HTTP请求）
     */
    void HandleRead();
    
    /**
     * @brief 处理写事件（发送HTTP响应数据）
     */
    void HandleWrite();
    
    /**
     * @brief 处理关闭事件（触发close_callback_）
     */
    void HandleClose();
    
    /**
     * @brief 处理错误事件（触发HandleClose）
     */
    void HandleError();

    // ========== 成员变量 ==========
    EventLoop* loop_;                      // 所属的从Reactor
    std::unique_ptr<Channel> channel_;     // 管理连接fd的Channel
    int fd_;                               // 连接fd
    std::string src_dir_;                  // 静态资源目录

    HttpRequest request_;                  // HTTP请求解析器
    HttpResponse response_;                // HTTP响应构建器
    std::string send_buffer_;              // 发送缓冲区（待发送的响应数据）
    CloseCallback close_callback_;         // 连接关闭回调（通知Server）
    Auth auth_;                            // 认证模块
    std::shared_ptr<CacheManager> cache_manager_; // 静态资源缓存管理器
};

} // namespace reactor