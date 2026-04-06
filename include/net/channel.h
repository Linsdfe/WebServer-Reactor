#pragma once

#include <functional>
#include <sys/epoll.h>

namespace reactor {

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    // 设置事件回调函数
    void SetReadCallback(EventCallback cb) { read_callback_ = std::move(cb); }
    void SetWriteCallback(EventCallback cb) { write_callback_ = std::move(cb); }
    void SetCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }
    void SetErrorCallback(EventCallback cb) { error_callback_ = std::move(cb); }

    // 事件处理入口
    void HandleEvent();

    // 基础属性获取
    int Fd() const { return fd_; }
    uint32_t Events() const { return events_; }
    void SetRevents(uint32_t revt) { revents_ = revt; }

    // 事件控制
    void EnableReading() { events_ |= EPOLLIN | EPOLLET; Update(); }
    void EnableWriting() { events_ |= EPOLLOUT | EPOLLET; Update(); }
    void DisableWriting() { events_ &= ~EPOLLOUT; Update(); }
    void DisableAll() { events_ = 0; Update(); }
    bool IsWriting() const { return events_ & EPOLLOUT; }

    // 从EventLoop中移除当前Channel
    void Remove();

private:
    void Update();

    EventLoop* loop_;
    const int fd_;
    uint32_t events_;  // 感兴趣的事件
    uint32_t revents_; // 实际触发的事件

    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};

} // namespace reactor
