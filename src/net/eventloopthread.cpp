/**
 * @file eventloopthreadpool.cpp
 * @brief EventLoopThreadPool类实现：管理IO线程池（从Reactor）
 * 
 * EventLoopThreadPool核心职责：
 * 1. 创建指定数量的EventLoopThread（IO线程）
 * 2. 启动所有IO线程，保存每个线程的EventLoop
 * 3. 轮询分配EventLoop（Round-Robin）给新连接
 */
#include "net/eventloopthreadpool.h"

namespace reactor {

/**
 * @brief 线程池构造函数
 * @param base_loop 主Reactor的EventLoop
 * @param thread_num IO线程数
 */
EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, int thread_num)
    : base_loop_(base_loop), started_(false), thread_num_(thread_num), next_(0) {}

/**
 * @brief 线程池析构函数（空实现，智能指针自动释放）
 */
EventLoopThreadPool::~EventLoopThreadPool() {}

/**
 * @brief 启动线程池（创建并启动所有IO线程）
 * 
 * 注意：必须在主Reactor线程执行（AssertInLoopThread）
 */
void EventLoopThreadPool::Start() {
    base_loop_->AssertInLoopThread(); // 确保线程安全
    started_ = true;

    // 创建thread_num_个IO线程，并保存每个线程的EventLoop
    for (int i = 0; i < thread_num_; ++i) {
        threads_.emplace_back(new EventLoopThread()); // 创建IO线程
        loops_.push_back(threads_.back()->StartLoop()); // 启动线程并获取EventLoop
    }
}

/**
 * @brief 轮询获取下一个IO线程的EventLoop（负载均衡）
 * @return EventLoop* 分配的EventLoop（无IO线程时返回base_loop_）
 * 
 * 注意：必须在主Reactor线程执行（AssertInLoopThread）
 */
EventLoop* EventLoopThreadPool::GetNextLoop() {
    base_loop_->AssertInLoopThread();
    EventLoop* loop = base_loop_; // 兜底：无IO线程时使用主Reactor

    // 轮询分配：next_递增，取模实现循环
    if (!loops_.empty()) {
        loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
    }
    return loop;
}

} // namespace reactor