#pragma once

/**
 * @file server.h
 * @brief Server 类：主服务器类，负责 Accept 新连接、分发给 IO 线程，以及管理活跃连接
 *
 * 【Reactor 架构位置】
 * - 是 **整个 Reactor WebServer 的入口**
 * - 整合主 Reactor、IO 线程池、Acceptor、连接管理
 * - 负责整个服务器的生命周期管理
 */

#include "net/eventloop.h"
#include "net/eventloopthreadpool.h"
#include "net/acceptor.h"
#include "server/tcpconnection.h"
#include <unordered_map>
#include <memory>
#include <string>

namespace reactor {

/**
 * @class Server
 * @brief 主服务器入口类
 *
 * 核心职责：
 * 1. 初始化主 Reactor、IO 线程池、Acceptor
 * 2. 定位静态资源目录（www）
 * 3. 接受新连接（Acceptor 回调），分配给 IO 线程
 * 4. 管理所有活跃连接（connections_ 映射表）
 * 5. 线程安全的连接创建/销毁
 */
class Server {
public:
    /**
     * @brief Server 构造函数
     * @param port 监听端口
     * @param thread_num IO 线程数（默认 8）
     *
     * 初始化流程：
     * 1. 创建主 Reactor（base_loop_）
     * 2. 创建 IO 线程池（thread_pool_）
     * 3. 创建 Acceptor（acceptor_）
     * 4. 定位静态资源目录（www）
     * 5. 设置 Acceptor 的新连接回调（NewConnection）
     */
    Server(int port, int thread_num = 8);

    /**
     * @brief Server 析构函数（空实现，智能指针自动释放）
     */
    ~Server();

    /**
     * @brief 启动服务器，进入主事件循环
     *
     * 启动流程：
     * 1. 启动 IO 线程池（创建所有从 Reactor）
     * 2. 主 Reactor 中启动 Acceptor 监听
     * 3. 启动主 Reactor 事件循环（Loop）
     */
    void Start();

private:
    /**
     * @brief 处理新连接（Acceptor 回调触发）
     * @param sockfd 新连接的 socket fd
     * @param addr 客户端地址
     *
     * 【线程安全】必须在主 Reactor 线程执行
     *
     * 核心逻辑：
     * 1. 轮询获取一个从 Reactor 的 EventLoop
     * 2. 创建 TcpConnection 对象，管理新连接
     * 3. 保存连接到 connections_ 映射表
     * 4. 设置连接关闭回调（RemoveConnection）
     * 5. 在从 Reactor 线程中初始化连接
     */
    void NewConnection(int sockfd, const struct sockaddr_in& addr);

    /**
     * @brief 触发连接移除（TcpConnection 回调触发）
     * @param sockfd 要关闭的连接 fd
     *
     * 【线程安全】可能在从 Reactor 线程调用，需转发到主 Reactor
     */
    void RemoveConnection(int sockfd);

    /**
     * @brief 实际执行连接移除（主 Reactor 线程）
     * @param sockfd 要关闭的连接 fd
     *
     * 【线程安全】必须在主 Reactor 线程执行
     *
     * 核心逻辑：
     * 1. 从 connections_ 映射表中移除连接
     * 2. 在从 Reactor 线程中销毁连接
     */
    void RemoveConnectionInLoop(int sockfd);

    std::unique_ptr<EventLoop> base_loop_;                      // 主 Reactor
    std::unique_ptr<EventLoopThreadPool> thread_pool_;          // IO 线程池
    std::unique_ptr<Acceptor> acceptor_;                        // Acceptor（监听端口）
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_; // 活跃连接映射表（fd → TcpConnection）
    std::string src_dir_;                                         // 静态资源目录（www）
    int port_;                                                     // 监听端口
};

} // namespace reactor