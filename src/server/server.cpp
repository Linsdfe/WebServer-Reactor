#include "server/server.h"
#include <unistd.h>
#include <iostream>

namespace reactor {

Server::Server(int port, int thread_num)
    : base_loop_(new EventLoop()),
      thread_pool_(new EventLoopThreadPool(base_loop_.get(), thread_num)),
      acceptor_(new Acceptor(base_loop_.get(), port)),
      port_(port) {
    
    // 获取可执行文件所在目录，定位www资源目录
    char path[256];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (len != -1) {
        path[len] = '\0';
        std::string exe_path(path);
        size_t pos = exe_path.find_last_of('/');
        src_dir_ = exe_path.substr(0, pos + 1) + "www/";
    } else {
        // 兜底：当前目录下的www
        src_dir_ = "./www/";
    }

    std::cout << "[Info] Resource directory: " << src_dir_ << std::endl;

    // 设置新连接回调
    acceptor_->SetNewConnectionCallback(
        std::bind(&Server::NewConnection, this, std::placeholders::_1, std::placeholders::_2)
    );
}

Server::~Server() {}

void Server::Start() {
    std::cout << "[Info] Server start success!" << std::endl;
    // 启动IO线程池
    thread_pool_->Start();
    // 主Reactor开始监听
    base_loop_->RunInLoop(std::bind(&Acceptor::Listen, acceptor_.get()));
    // 启动主事件循环
    base_loop_->Loop();
}

void Server::NewConnection(int sockfd, const struct sockaddr_in& addr) {
    (void)addr; // 显式标记未使用参数
    base_loop_->AssertInLoopThread();
    // 轮询获取一个IO线程的EventLoop
    EventLoop* io_loop = thread_pool_->GetNextLoop();

    // 创建TCP连接对象
    std::shared_ptr<TcpConnection> conn = std::make_shared<TcpConnection>(io_loop, sockfd, src_dir_);
    connections_[sockfd] = conn;

    // 设置连接关闭回调
    conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

    // 在IO线程中完成连接初始化
    io_loop->RunInLoop(std::bind(&TcpConnection::ConnectEstablished, conn));
}

void Server::RemoveConnection(int sockfd) {
    // 必须在主Reactor线程中执行，保证线程安全
    base_loop_->RunInLoop(std::bind(&Server::RemoveConnectionInLoop, this, sockfd));
}

void Server::RemoveConnectionInLoop(int sockfd) {
    base_loop_->AssertInLoopThread();
    auto it = connections_.find(sockfd);
    if (it != connections_.end()) {
        std::shared_ptr<TcpConnection> conn = it->second;
        connections_.erase(it);
        // 在IO线程中销毁连接
        EventLoop* io_loop = conn->GetLoop();
        io_loop->RunInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
    }
}

} // namespace reactor
