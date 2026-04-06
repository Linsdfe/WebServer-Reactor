#include "net/acceptor.h"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <netinet/tcp.h> 

namespace reactor {

// 创建非阻塞监听fd
static int CreateListenFd(int port) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("[Fatal] socket create failed");
        exit(EXIT_FAILURE);
    }

    // 端口复用
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Fatal] bind failed");
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    // 开始监听
    if (listen(listenfd, 1024) < 0) {
        perror("[Fatal] listen failed");
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    // 设置非阻塞
    fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL) | O_NONBLOCK);
    return listenfd;
}

Acceptor::Acceptor(EventLoop* loop, int port)
    : loop_(loop), listenfd_(CreateListenFd(port)),
      accept_channel_(loop, listenfd_), listening_(false), port_(port) {
    // 注册读事件回调（接受新连接）
    accept_channel_.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
}

Acceptor::~Acceptor() {
    accept_channel_.DisableAll();
    accept_channel_.Remove();
    close(listenfd_);
}

void Acceptor::Listen() {
    loop_->AssertInLoopThread();
    listening_ = true;
    accept_channel_.EnableReading();
    std::cout << "[Info] Server listening on port " << port_ << std::endl;
}

void Acceptor::HandleRead() {
    loop_->AssertInLoopThread();
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // ET模式下必须循环accept，直到没有新连接
    while (true) {
        int connfd = accept(listenfd_, (struct sockaddr*)&client_addr, &addr_len);
        if (connfd < 0) {
            // 没有新连接了，退出循环
            break;
        }

        // 设置新连接为非阻塞+关闭Nagle算法
        fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL) | O_NONBLOCK);
        int optval = 1;
        setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

        // 回调上层处理新连接
        if (new_connection_callback_) {
            new_connection_callback_(connfd, client_addr);
        } else {
            close(connfd);
        }
    }
}

} // namespace reactor
