#include "net/channel.h"
#include "net/eventloop.h"

namespace reactor {

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0) {}

Channel::~Channel() {}

void Channel::HandleEvent() {
    // 错误事件处理
    if (revents_ & EPOLLERR) {
        if (error_callback_) error_callback_();
    }
    // 连接关闭事件
    if (revents_ & (EPOLLHUP | EPOLLRDHUP)) {
        if (close_callback_) close_callback_();
    }
    // 可读事件
    if (revents_ & EPOLLIN) {
        if (read_callback_) read_callback_();
    }
    // 可写事件
    if (revents_ & EPOLLOUT) {
        if (write_callback_) write_callback_();
    }
}

void Channel::Update() {
    loop_->UpdateChannel(this);
}

void Channel::Remove() {
    loop_->RemoveChannel(this);
}

} // namespace reactor
