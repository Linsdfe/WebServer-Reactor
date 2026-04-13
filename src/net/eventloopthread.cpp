/**
 * @file eventloopthread.cpp
 * @brief EventLoopThread 类实现：单个 IO 线程封装，遵循 one loop per thread 设计思想
 *
 * 【Reactor 架构位置】
 * - 属于从 Reactor 组件，是 IO 线程池的最小工作单元
 * - 核心作用是创建独立线程，在线程内绑定并运行一个 EventLoop 事件循环
 */

#include "net/eventloopthread.h"

namespace reactor {

/**
 * @brief EventLoopThread 构造函数
 *
 * 初始化内容：
 * - 将事件循环指针 loop_ 初始化为空
 */
EventLoopThread::EventLoopThread() : loop_(nullptr) {}

/**
 * @brief EventLoopThread 析构函数：线程安全回收 IO 线程资源
 *
 * 资源清理流程：
 * 1. 若事件循环有效，主动调用 Quit() 退出事件循环
 * 2. 若线程可等待，调用 join() 等待线程执行完毕
 * 3. 避免线程泄漏、野指针等问题
 */
EventLoopThread::~EventLoopThread() {
    // 如果事件循环有效，主动退出循环
    if (loop_) {
        loop_->Quit();
        // 如果线程可等待，等待线程执行完毕（避免线程泄漏）
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

/**
 * @brief 启动 IO 线程，并同步获取线程内的 EventLoop 指针
 * @return EventLoop* 子线程中运行的事件循环对象指针
 *
 * 【线程安全】使用互斥锁+条件变量实现主线程与子线程的同步
 *
 * 执行流程：
 * 1. 创建子线程，绑定入口函数 ThreadFunc
 * 2. 主线程阻塞等待，直到子线程完成 EventLoop 初始化
 * 3. 子线程初始化完成后，返回事件循环指针
 */
EventLoop* EventLoopThread::StartLoop() {
    // 创建子线程，执行线程入口函数ThreadFunc
    thread_ = std::thread(&EventLoopThread::ThreadFunc, this);

    EventLoop* loop = nullptr;
    {
        // 加互斥锁，保护共享变量loop_的访问
        std::unique_lock<std::mutex> lock(mutex_);
        // 【核心】等待子线程完成EventLoop的初始化
        while (loop_ == nullptr) {
            // 条件变量等待，自动释放锁，被唤醒后重新加锁
            cond_.wait(lock);
        }
        // 子线程初始化完成，赋值loop指针
        loop = loop_;
    }
    // 返回线程中的事件循环对象
    return loop;
}

/**
 * @brief 子线程入口函数（完全运行在新创建的 IO 线程中）
 *
 * 执行流程：
 * 1. 在子线程栈上创建 EventLoop 对象（一个线程唯一绑定一个事件循环）
 * 2. 加锁赋值 loop_，并通过条件变量通知主线程初始化完成
 * 3. 启动事件循环（阻塞运行，直到调用 Quit() 退出）
 * 4. 循环退出后重置 loop_ 指针，防止野指针
 */
void EventLoopThread::ThreadFunc() {
    // 在【子线程栈上】创建EventLoop对象（一个线程对应一个EventLoop）
    EventLoop loop;
    {
        // 加锁，保证loop_赋值的原子性
        std::lock_guard<std::mutex> lock(mutex_);
        // 将成员变量loop_指向当前线程创建的EventLoop
        loop_ = &loop;
        // 【唤醒】主线程：通知loop初始化完成
        cond_.notify_one();
    }

    // 【核心】启动事件循环（阻塞运行，直到调用Quit()）
    loop.Loop();
    // 事件循环退出后，重置指针（防止野指针）
    loop_ = nullptr;
}

} // namespace reactor