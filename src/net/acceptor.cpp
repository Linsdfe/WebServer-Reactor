/**
 * @file acceptor.cpp
 * @brief Acceptor类实现：负责监听端口、接受新连接
 * 
 * Acceptor核心职责：
 * 1. 创建并初始化监听fd（非阻塞、端口复用、TCP_NODELAY）
 * 2. 注册监听fd到主Reactor的Epoller（读事件）
 * 3. 接受新连接（ET模式下循环accept）
 * 4. 触发新连接回调（通知Server处理）
 */
#include "net/acceptor.h"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <netinet/tcp.h> 

namespace reactor {

/**
 * @brief 创建并初始化监听fd
 * @param port 监听端口
 * @return int 初始化后的监听fd（失败则退出进程）
 * 
 * 初始化步骤：
 * 1. 创建TCP socket（AF_INET/SOCK_STREAM）
 * 2. 设置端口复用（SO_REUSEADDR/SO_REUSEPORT）
 * 3. 绑定地址（INADDR_ANY:port）
 * 4. 开始监听（backlog=1024）
 * 5. 设置为非阻塞模式（ET模式必须）
 */
static int CreateListenFd(int port) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("[Fatal] socket create failed");
        exit(EXIT_FAILURE);
    }

    // 端口复用：避免服务器重启后端口占用
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    // 绑定地址：监听所有网卡的port端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 任意IP地址
    addr.sin_port = htons(port); // 网络字节序端口

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Fatal] bind failed");
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    // 开始监听：backlog=1024（未完成连接队列大小）
    if (listen(listenfd, 1024) < 0) {
        perror("[Fatal] listen failed");
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    // 设置非阻塞：ET模式下必须使用非阻塞fd
    fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL) | O_NONBLOCK);
    return listenfd;
}

/**
 * @brief Acceptor构造函数
 * @param loop 所属的EventLoop（主Reactor）
 * @param port 监听端口
 * 
 * 初始化流程：
 * 1. 创建监听fd（CreateListenFd）
 * 2. 创建Channel管理监听fd
 * 3. 设置读事件回调（HandleRead：接受新连接）
 */
Acceptor::Acceptor(EventLoop* loop, int port)
    : loop_(loop), listenfd_(CreateListenFd(port)),
      accept_channel_(loop, listenfd_), listening_(false), port_(port) {
    // 注册读事件回调：监听fd可读时（有新连接）触发HandleRead
    accept_channel_.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
}

/**
 * @brief Acceptor析构函数
 * 
 * 清理步骤：
 * 1. 禁用所有事件
 * 2. 从Epoller移除Channel
 * 3. 关闭监听fd
 */
Acceptor::~Acceptor() {
    accept_channel_.DisableAll();
    accept_channel_.Remove();
    close(listenfd_);
}

/**
 * @brief 启动监听（注册监听fd到Epoller）
 * 
 * 注意：必须在主Reactor线程执行（AssertInLoopThread）
 */
void Acceptor::Listen() {
    loop_->AssertInLoopThread(); // 确保线程安全
    listening_ = true;
    accept_channel_.EnableReading(); // 开启读事件（EPOLLIN+EPOLLET）
    std::cout << "[Info] Server listening on port " << port_ << std::endl;
}

/**
 * @brief 处理新连接（读事件回调）
 * 
 * 核心逻辑：
 * 1. 断言：必须在主Reactor线程执行
 * 2. ET模式下循环accept（直到EAGAIN）
 * 3. 新连接fd初始化（非阻塞、关闭Nagle算法）
 * 4. 触发新连接回调（通知Server处理）
 */
void Acceptor::HandleRead() {
    loop_->AssertInLoopThread();
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // ET模式下必须循环accept：一次读取所有待处理连接
    while (true) {
        int connfd = accept(listenfd_, (struct sockaddr*)&client_addr, &addr_len);
        if (connfd < 0) {
            // 没有新连接：EAGAIN/EWOULDBLOCK（非阻塞accept的正常返回）
            break;
        }

        // 新连接fd初始化：非阻塞+关闭Nagle（降低延迟）
        fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL) | O_NONBLOCK);
        int optval = 1;
        setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

        // 触发回调：通知Server处理新连接
        if (new_connection_callback_) {
            new_connection_callback_(connfd, client_addr);
        } else {
            // 无回调时关闭fd（避免资源泄漏）
            close(connfd);
        }
    }
}

} // namespace reactor