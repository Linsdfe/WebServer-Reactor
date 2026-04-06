#include "net/eventloopthread.h"

namespace reactor {

EventLoopThread::EventLoopThread() : loop_(nullptr) {}

EventLoopThread::~EventLoopThread() {
    if (loop_) {
        loop_->Quit();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

EventLoop* EventLoopThread::StartLoop() {
    thread_ = std::thread(&EventLoopThread::ThreadFunc, this);

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待线程初始化完成
        while (loop_ == nullptr) {
            cond_.wait(lock);
        }
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::ThreadFunc() {
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }
    // 启动事件循环
    loop.Loop();
    // 循环退出后重置指针
    loop_ = nullptr;
}

} // namespace reactor
