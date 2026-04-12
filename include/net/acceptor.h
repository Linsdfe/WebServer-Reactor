#pragma once

/**
 * @file acceptor.h
 * @brief Acceptor 类：负责创建监听 socket、接受新连接，并将新连接分发给主 Reactor
 *
 * 【Reactor 架构位置】
 * - 属于**主 Reactor（Base Reactor）**的核心组件
 * - 仅负责监听端口、接受新连接，不处理具体 IO
 * - 新连接通过回调分发给 Server，再由 Server 分配给从 Reactor（IO 线程）
 */

#include "net/eventloop.h"
#include "net/channel.h"
#include <functional>
#include <arpa/inet.h>

namespace reactor {

/**
 * @class Acceptor
 * @brief 监听端口、接受新连接的封装类
 *
 * 核心职责：
 * 1. 创建并初始化监听 fd（非阻塞、端口复用、TCP_NODELAY）
 * 2. 注册监听 fd 到主 Reactor 的 Epoller（读事件）
 * 3. ET 模式下循环 accept，接受所有待处理新连接
 * 4. 触发新连接回调，将 fd 交给 Server 分配给 IO 线程
 */
class Acceptor {
public:
    /**
     * @brief 新连接回调类型
     * @param sockfd 新连接的 socket fd
     * @param addr 客户端地址（sockaddr_in 结构体）
     */
    using NewConnectionCallback = std::function<void(int sockfd, const struct sockaddr_in&)>;

    /**
     * @brief Acceptor 构造函数
     * @param loop 所属的主 Reactor（EventLoop）
     * @param port 监听端口
     *
     * 初始化流程：
     * 1. 创建监听 fd（CreateListenFd）
     * 2. 创建 Channel 管理监听 fd
     * 3. 设置读事件回调（HandleRead）
     */
    Acceptor(EventLoop* loop, int port);

    /**
     * @brief Acceptor 析构函数
     *
     * 清理流程：
     * 1. 禁用监听 fd 的所有事件
     * 2. 从 Epoller 移除 Channel
     * 3. 关闭监听 fd
     */
    ~Acceptor();

    /**
     * @brief 设置新连接回调（Server 调用）
     * @param cb 新连接回调函数
     */
    void SetNewConnectionCallback(NewConnectionCallback cb) { new_connection_callback_ = std::move(cb); }

    /**
     * @brief 启动监听（注册监听 fd 到 Epoller）
     *
     * 【线程安全】必须在主 Reactor 线程执行
     */
    void Listen();

private:
    /**
     * @brief 处理监听 fd 的读事件（接受新连接）
     *
     * 核心逻辑：
     * 1. ET 模式下循环 accept（直到 EAGAIN）
     * 2. 新连接 fd 初始化（非阻塞、TCP_NODELAY）
     * 3. 触发新连接回调（通知 Server）
     */
    void HandleRead();

    EventLoop* loop_;                      // 所属的主 Reactor
    int listenfd_;                         // 监听 socket fd
    Channel accept_channel_;               // 管理 listenfd_ 的 Channel
    NewConnectionCallback new_connection_callback_; // 新连接回调
    bool listening_;                       // 是否正在监听
    int port_;                             // 监听端口
};

} // namespace reactor