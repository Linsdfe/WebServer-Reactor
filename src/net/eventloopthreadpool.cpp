#include "net/eventloopthreadpool.h"

namespace reactor {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, int thread_num)
    : base_loop_(base_loop), started_(false), thread_num_(thread_num), next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() {}

void EventLoopThreadPool::Start() {
    base_loop_->AssertInLoopThread();
    started_ = true;

    // 创建指定数量的IO线程
    for (int i = 0; i < thread_num_; ++i) {
        threads_.emplace_back(new EventLoopThread());
        loops_.push_back(threads_.back()->StartLoop());
    }
}

EventLoop* EventLoopThreadPool::GetNextLoop() {
    base_loop_->AssertInLoopThread();
    EventLoop* loop = base_loop_;

    // 轮询分配IO线程
    if (!loops_.empty()) {
        loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
    }
    return loop;
}

} // namespace reactor
