#include "net/eventloop.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <iostream>

namespace reactor {

// 创建eventfd用于跨线程唤醒EventLoop
static int CreateEventFd() {
    int evtfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        std::cerr << "[Error] Failed to create eventfd" << std::endl;
        abort();
    }
    return evtfd;
}

// 【关键修复】初始化列表顺序必须与头文件声明顺序完全一致
EventLoop::EventLoop()
    : thread_id_(std::this_thread::get_id()), // 1. 先初始化 thread_id_
      epoller_(new Epoller()),                 // 2. 再初始化 epoller_
      looping_(false),                         // 3. 然后是 looping_
      quit_(false),                            // 4. 然后是 quit_
      wakeup_fd_(CreateEventFd()),             // 5. 后续顺序随意
      wakeup_channel_(new Channel(this, wakeup_fd_)),
      calling_pending_functors_(false) {
    wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
    wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
    wakeup_channel_->DisableAll();
    wakeup_channel_->Remove();
    close(wakeup_fd_);
}

void EventLoop::Loop() {
    AssertInLoopThread();
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        std::vector<Channel*> active_channels;
        epoller_->Wait(100, active_channels);

        // 处理就绪事件
        for (Channel* channel : active_channels) {
            channel->HandleEvent();
        }

        // 执行跨线程提交的任务
        DoPendingFunctors();
    }

    looping_ = false;
}

void EventLoop::Quit() {
    quit_ = true;
    // 如果不在当前线程，需要唤醒EventLoop
    if (!IsInLoopThread()) {
        Wakeup();
    }
}

void EventLoop::UpdateChannel(Channel* channel) {
    AssertInLoopThread();
    epoller_->UpdateChannel(channel);
}

void EventLoop::RemoveChannel(Channel* channel) {
    AssertInLoopThread();
    epoller_->RemoveChannel(channel);
}

void EventLoop::RunInLoop(Functor cb) {
    if (IsInLoopThread()) {
        cb();
    } else {
        QueueInLoop(std::move(cb));
    }
}

void EventLoop::QueueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.push_back(std::move(cb));
    }
    // 不在当前线程 或 正在执行任务队列，需要唤醒
    if (!IsInLoopThread() || calling_pending_functors_) {
        Wakeup();
    }
}

void EventLoop::Wakeup() {
    uint64_t one = 1;
    ssize_t n = write(wakeup_fd_, &one, sizeof(one));
    (void)n; // 忽略返回值，避免编译警告
}

void EventLoop::HandleRead() {
    uint64_t one = 1;
    ssize_t n = read(wakeup_fd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::DoPendingFunctors() {
    std::vector<Functor> functors;
    calling_pending_functors_ = true;

    // 用swap减少锁的持有时间
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    // 执行任务
    for (const Functor& functor : functors) {
        functor();
    }

    calling_pending_functors_ = false;
}

void EventLoop::AbortNotInLoopThread() {
    std::cerr << "[Fatal] EventLoop is not in its creation thread!" << std::endl;
    abort();
}

} // namespace reactor
