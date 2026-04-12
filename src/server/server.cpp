/**
 * @file server.cpp
 * @brief 服务器核心类实现：主Reactor管理监听、连接分发，从Reactor处理IO
 * 
 * Server类是整个Reactor架构的入口，负责：
 * 1. 初始化主EventLoop、IO线程池、Acceptor
 * 2. 管理TCP连接生命周期（创建/销毁）
 * 3. 线程安全的连接映射管理
 */
#include "server/server.h"
#include <unistd.h>
#include <iostream>

namespace reactor {

/**
 * @brief Server构造函数
 * @param port 监听端口
 * @param thread_num IO线程数（最终传给EventLoopThreadPool）
 * 
 * 初始化流程：
 * 1. 创建主Reactor（base_loop_）
 * 2. 创建IO线程池（thread_pool_）
 * 3. 创建Acceptor（负责监听端口、接受新连接）
 * 4. 定位静态资源目录（优先可执行文件目录，兜底当前目录）
 * 5. 设置新连接回调函数
 */
Server::Server(int port, int thread_num)
    : base_loop_(new EventLoop()),
      thread_pool_(new EventLoopThreadPool(base_loop_.get(), thread_num)),
      acceptor_(new Acceptor(base_loop_.get(), port)),
      port_(port) {
    
    // ========== 定位静态资源目录（www） ==========
    char path[256];
    // 读取当前进程可执行文件的绝对路径（/proc/self/exe是符号链接）
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (len != -1) {
        path[len] = '\0';
        std::string exe_path(path);
        // 截取可执行文件所在目录（去掉文件名）
        size_t pos = exe_path.find_last_of('/');
        src_dir_ = exe_path.substr(0, pos + 1) + "www/";
    } else {
        // 兜底策略：当前工作目录下的www
        src_dir_ = "./www/";
    }

    std::cout << "[Info] Resource directory: " << src_dir_ << std::endl;

    // ========== 设置新连接回调 ==========
    // 绑定NewConnection到Acceptor的回调，Acceptor接受连接后触发
    acceptor_->SetNewConnectionCallback(
        std::bind(&Server::NewConnection, this, std::placeholders::_1, std::placeholders::_2)
    );
}

/**
 * @brief Server析构函数（空实现，智能指针自动释放资源）
 */
Server::~Server() {}

/**
 * @brief 启动服务器核心流程
 * 
 * 启动步骤：
 * 1. 启动IO线程池（创建并启动所有从Reactor线程）
 * 2. 主Reactor中启动Acceptor的监听（注册listenfd到epoll）
 * 3. 启动主Reactor的事件循环（base_loop_->Loop()）
 */
void Server::Start() {
    std::cout << "[Info] Server start success!" << std::endl;
    // 启动IO线程池：创建thread_num个EventLoopThread并启动
    thread_pool_->Start();
    // 主Reactor线程中执行Acceptor::Listen（保证线程安全）
    base_loop_->RunInLoop(std::bind(&Acceptor::Listen, acceptor_.get()));
    // 启动主Reactor的事件循环（阻塞，直到退出）
    base_loop_->Loop();
}

/**
 * @brief 处理新连接（Acceptor回调触发）
 * @param sockfd 新连接的socket fd
 * @param addr 客户端地址（当前未使用，保留扩展）
 * 
 * 核心逻辑：
 * 1. 断言：必须在主Reactor线程执行（Acceptor的回调在主Reactor）
 * 2. 轮询获取一个从Reactor的EventLoop（负载均衡）
 * 3. 创建TcpConnection对象，管理新连接
 * 4. 设置连接关闭回调，关联到Server::RemoveConnection
 * 5. 在从Reactor线程中初始化连接（注册fd到epoll）
 */
void Server::NewConnection(int sockfd, const struct sockaddr_in& addr) {
    (void)addr; // 显式标记未使用参数，避免编译警告
    base_loop_->AssertInLoopThread(); // 确保在主Reactor线程执行
    
    // 轮询获取下一个IO线程的EventLoop（Round-Robin负载均衡）
    EventLoop* io_loop = thread_pool_->GetNextLoop();

    // 创建TCP连接对象：托管到智能指针，保证生命周期
    std::shared_ptr<TcpConnection> conn = std::make_shared<TcpConnection>(io_loop, sockfd, src_dir_);
    // 保存连接到映射表，管理所有活跃连接
    connections_[sockfd] = conn;

    // 设置连接关闭回调：连接关闭时通知Server清理
    conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

    // 在IO线程中初始化连接（注册fd到epoll，开启读事件）
    io_loop->RunInLoop(std::bind(&TcpConnection::ConnectEstablished, conn));
}

/**
 * @brief 触发连接移除（TcpConnection回调触发）
 * @param sockfd 要关闭的连接fd
 * 
 * 注意：
 * 1. 该函数可能在从Reactor线程调用，需转发到主Reactor线程执行
 * 2. 保证连接管理的线程安全（所有连接操作在主Reactor）
 */
void Server::RemoveConnection(int sockfd) {
    // 转发到主Reactor线程执行，避免多线程竞争connections_
    base_loop_->RunInLoop(std::bind(&Server::RemoveConnectionInLoop, this, sockfd));
}

/**
 * @brief 实际执行连接移除（主Reactor线程）
 * @param sockfd 要关闭的连接fd
 * 
 * 核心逻辑：
 * 1. 断言：必须在主Reactor线程执行
 * 2. 从映射表中移除连接，获取智能指针（延长生命周期）
 * 3. 在从Reactor线程中销毁连接（注销epoll、关闭fd）
 */
void Server::RemoveConnectionInLoop(int sockfd) {
    base_loop_->AssertInLoopThread(); // 确保线程安全
    auto it = connections_.find(sockfd);
    if (it != connections_.end()) {
        std::shared_ptr<TcpConnection> conn = it->second;
        connections_.erase(it); // 从映射表移除
        
        // 在IO线程中销毁连接（注销epoll事件、关闭fd）
        EventLoop* io_loop = conn->GetLoop();
        io_loop->RunInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
    }
}

} // namespace reactor