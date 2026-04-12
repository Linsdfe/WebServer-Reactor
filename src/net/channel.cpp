/**
 * @file channel.cpp
 * @brief Channel类实现：封装fd和事件回调，是EventLoop和fd的桥梁
 * 
 * Channel核心职责：
 * 1. 管理单个fd的感兴趣事件（events_）和实际触发事件（revents_）
 * 2. 注册事件回调（读/写/关闭/错误）
 * 3. 触发事件回调（HandleEvent）
 * 4. 事件的增删改（Update/Remove）
 */
#include "net/channel.h"
#include "net/eventloop.h"

namespace reactor {

/**
 * @brief Channel构造函数
 * @param loop 所属的EventLoop
 * @param fd 关联的文件描述符（socket fd）
 */
Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0) {}

/**
 * @brief Channel析构函数（空实现，资源由上层管理）
 */
Channel::~Channel() {}

/**
 * @brief 事件处理入口：根据revents_触发对应回调
 * 
 * 事件处理顺序（优先级）：
 * 1. 错误事件（EPOLLERR）→ error_callback_
 * 2. 关闭事件（EPOLLHUP/EPOLLRDHUP）→ close_callback_
 * 3. 可读事件（EPOLLIN）→ read_callback_
 * 4. 可写事件（EPOLLOUT）→ write_callback_
 */
void Channel::HandleEvent() {
    // 错误事件处理（最高优先级）
    if (revents_ & EPOLLERR) {
        if (error_callback_) error_callback_();
    }
    // 连接关闭事件（TCP连接正常关闭/半关闭）
    if (revents_ & (EPOLLHUP | EPOLLRDHUP)) {
        if (close_callback_) close_callback_();
    }
    // 可读事件（数据到达/连接建立）
    if (revents_ & EPOLLIN) {
        if (read_callback_) read_callback_();
    }
    // 可写事件（发送缓冲区有空余）
    if (revents_ & EPOLLOUT) {
        if (write_callback_) write_callback_();
    }
}

/**
 * @brief 更新事件到Epoller（封装epoll_ctl）
 * 
 * 调用所属EventLoop的UpdateChannel，最终触发Epoller::UpdateChannel
 */
void Channel::Update() {
    loop_->UpdateChannel(this);
}

/**
 * @brief 从Epoller中移除当前Channel
 * 
 * 调用所属EventLoop的RemoveChannel，最终触发Epoller::RemoveChannel
 */
void Channel::Remove() {
    loop_->RemoveChannel(this);
}

} // namespace reactor