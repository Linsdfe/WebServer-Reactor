#pragma once

/**
 * @file eventloopthreadpool.h
 * @brief EventLoopThreadPool 类：IO 线程池，创建多个 EventLoopThread，负责分发客户端连接到不同 IO 线程
 *
 * 【Reactor 架构位置】
 * - 属于 **主 Reactor（Base Reactor）**的组件
 * - 管理所有从 Reactor（IO 线程）
 * - 负责新连接的负载均衡（轮询分配给 IO 线程）
 */

#include "net/eventloop.h"
#include "net/eventloopthread.h"
#include <vector>
#include <memory>

namespace reactor {

/**
 * @class EventLoopThreadPool
 * @brief IO 线程池封装类
 *
 * 核心职责：
 * 1. 创建指定数量的 EventLoopThread（IO 线程）
 * 2. 启动所有 IO 线程，保存每个线程的 EventLoop 指针
 * 3. 轮询（Round-Robin）分配 EventLoop 给新连接（负载均衡）
 * 4. 析构时自动清理所有 IO 线程
 */
class EventLoopThreadPool {
public:
    /**
     * @brief EventLoopThreadPool 构造函数
     * @param base_loop 主 Reactor 的 EventLoop
     * @param thread_num IO 线程数（从 Reactor 数量）
     */
    EventLoopThreadPool(EventLoop* base_loop, int thread_num);

    /**
     * @brief EventLoopThreadPool 析构函数（空实现，智能指针自动释放）
     */
    ~EventLoopThreadPool();

    /**
     * @brief 启动线程池（创建并启动所有 IO 线程）
     *
     * 【线程安全】必须在主 Reactor 线程执行
     */
    void Start();

    /**
     * @brief 轮询获取下一个 IO 线程的 EventLoop（负载均衡）
     * @return EventLoop* 分配的 EventLoop（无 IO 线程时返回 base_loop_）
     *
     * 【线程安全】必须在主 Reactor 线程执行
     */
    EventLoop* GetNextLoop();

    /**
     * @brief 获取IO线程数量
     * @return int IO线程数量
     */
    int GetThreadNum() const { return thread_num_; }

private:
    EventLoop* base_loop_;                      // 主 Reactor 的 EventLoop
    bool started_;                               // 线程池是否已启动
    int thread_num_;                             // IO 线程数
    std::vector<std::unique_ptr<EventLoopThread>> threads_; // IO 线程数组
    std::vector<EventLoop*> loops_;              // 每个 IO 线程的 EventLoop 指针
    int next_;                                    // 轮询索引（Round-Robin）
};

} // namespace reactor