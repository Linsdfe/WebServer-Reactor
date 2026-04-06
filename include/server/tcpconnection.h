#pragma once

#include "net/eventloop.h"
#include "net/channel.h"
#include "http/httprequest.h"
#include "http/httpresponse.h"
#include <memory>
#include <string>

namespace reactor {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using CloseCallback = std::function<void(int)>;

    TcpConnection(EventLoop* loop, int fd, const std::string& src_dir);
    ~TcpConnection();

    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }
    void ConnectEstablished();
    void ConnectDestroyed();
    
    // 【修复】添加loop_的公有Getter方法
    EventLoop* GetLoop() const { return loop_; }

private:
    void HandleRead();
    void HandleWrite();
    void HandleClose();
    void HandleError();

    EventLoop* loop_;
    std::unique_ptr<Channel> channel_;
    int fd_;
    std::string src_dir_;

    HttpRequest request_;
    HttpResponse response_;
    std::string send_buffer_;
    CloseCallback close_callback_;
};

} // namespace reactor
