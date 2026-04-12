/**
 * @file eventloopthreadpool.cpp
 * @brief EventLoopThreadPool 类实现：IO 线程池，管理多个 EventLoopThread 并轮询分配连接
 *
 * 【Reactor 架构位置】
 * - 属于主 Reactor 组件，负责管理所有从 Reactor（IO 线程）
 * - 核心作用是通过轮询（Round-Robin）实现新连接的负载均衡
 */

#include "net/eventloopthreadpool.h"

namespace reactor {

/**
 * @brief EventLoopThreadPool 构造函数
 * @param base_loop 主 Reactor 的 EventLoop
 * @param thread_num IO 线程数（从 Reactor 数量）
 *
 * 初始化内容：
 * - 保存主 Reactor 指针
 * - 初始化启动状态为 false
 * - 保存 IO 线程数
 * - 初始化轮询索引为 0
 */
EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, int thread_num)
    : base_loop_(base_loop), started_(false), thread_num_(thread_num), next_(0) {}

/**
 * @brief EventLoopThreadPool 析构函数（空实现，智能指针自动释放资源）
 *
 * 资源清理：
 * - threads_ 中的 EventLoopThread 由 unique_ptr 自动析构
 * - 析构时会自动 join 所有 IO 线程（EventLoopThread 析构函数处理）
 */
EventLoopThreadPool::~EventLoopThreadPool() {}

/**
 * @brief 启动线程池（创建并启动所有 IO 线程）
 *
 * 【线程安全】必须在主 Reactor 线程执行（AssertInLoopThread 保证）
 *
 * 执行流程：
 * 1. 断言检查：确保在主 Reactor 线程
 * 2. 设置启动状态为 true
 * 3. 循环创建 thread_num_ 个 EventLoopThread
 * 4. 启动每个 IO 线程，保存其 EventLoop 指针到 loops_
 */
void EventLoopThreadPool::Start() {
    base_loop_->AssertInLoopThread();
    started_ = true;

    // 创建指定数量的IO线程
    for (int i = 0; i < thread_num_; ++i) {
        threads_.emplace_back(new EventLoopThread());
        loops_.push_back(threads_.back()->StartLoop());
    }
}

/**
 * @brief 轮询获取下一个 IO 线程的 EventLoop（负载均衡）
 * @return EventLoop* 分配的 EventLoop（无 IO 线程时返回主 Reactor）
 *
 * 【线程安全】必须在主 Reactor 线程执行（AssertInLoopThread 保证）
 *
 * 负载均衡逻辑（Round-Robin）：
 * 1. 默认返回主 Reactor（兜底，无 IO 线程时使用）
 * 2. 若有 IO 线程，按轮询索引 next_ 分配
 * 3. next_ 递增后取模，实现循环分配
 */
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