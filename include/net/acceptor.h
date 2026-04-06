#pragma once

#include "net/eventloop.h"
#include "net/channel.h"
#include <functional>
#include <arpa/inet.h>

namespace reactor {

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const struct sockaddr_in&)>;

    Acceptor(EventLoop* loop, int port);
    ~Acceptor();

    void SetNewConnectionCallback(NewConnectionCallback cb) { new_connection_callback_ = std::move(cb); }
    void Listen();

private:
    void HandleRead();

    EventLoop* loop_;
    int listenfd_;
    Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listening_;
    int port_;
};

} // namespace reactor
