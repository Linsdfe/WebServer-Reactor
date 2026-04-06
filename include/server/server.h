#pragma once

#include "net/eventloop.h"
#include "net/eventloopthreadpool.h"
#include "net/acceptor.h"
#include "server/tcpconnection.h"
#include <unordered_map>
#include <memory>
#include <string>

namespace reactor {

class Server {
public:
    Server(int port, int thread_num = 8);
    ~Server();

    void Start();

private:
    void NewConnection(int sockfd, const struct sockaddr_in& addr);
    void RemoveConnection(int sockfd);
    void RemoveConnectionInLoop(int sockfd);

    std::unique_ptr<EventLoop> base_loop_;
    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    std::string src_dir_;
    int port_;
};

} // namespace reactor
