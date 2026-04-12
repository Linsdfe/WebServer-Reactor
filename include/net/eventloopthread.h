#pragma once

/**
 * @file eventloopthread.h
 * @brief EventLoopThread 类：封装一条工作线程及其内部的 EventLoop
 *
 * 【Reactor 架构位置】
 * - 是 **IO 线程池（EventLoopThreadPool）的最小单元**
 * - 每个 EventLoopThread 对应一条 IO 线程和一个从 Reactor（EventLoop）
 * - 负责线程的创建、启动、同步，以及 EventLoop 的生命周期管理
 */

#include "net/eventloop.h"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace reactor {

/**
 * @class EventLoopThread
 * @brief 工作线程及其内部 EventLoop 的封装类
 *
 * 核心职责：
 * 1. 创建并启动工作线程
 * 2. 线程内创建 EventLoop（从 Reactor）
 * 3. 通过 mutex + condition_variable 实现线程同步（StartLoop 等待 EventLoop 创建完成）
 * 4. 析构时自动 join 线程，避免资源泄漏
 */
class EventLoopThread {
public:
    /**
     * @brief EventLoopThread 构造函数
     */
    EventLoopThread();

    /**
     * @brief EventLoopThread 析构函数
     *
     * 清理流程：
     * 1. 退出 EventLoop 事件循环
     * 2. join 工作线程（若可 join）
     */
    ~EventLoopThread();

    /**
     * @brief 启动线程并返回内部 EventLoop 指针
     * @return EventLoop* 线程内的 EventLoop（从 Reactor）
     *
     * 【同步机制】
     * - 调用线程阻塞，直到工作线程创建完 EventLoop
     * - 通过 mutex + condition_variable 实现同步
     */
    EventLoop* StartLoop();

private:
    /**
     * @brief 线程入口函数
     *
     * 执行流程：
     * 1. 创建 EventLoop
     * 2. 通知 StartLoop 线程（condition_variable::notify_one）
     * 3. 启动 EventLoop 事件循环（Loop）
     * 4. 循环退出后清理 EventLoop 指针
     */
    void ThreadFunc();

    EventLoop* loop_;                      // 线程内的 EventLoop（从 Reactor）
    std::thread thread_;                   // 工作线程
    std::mutex mutex_;                     // 保护 loop_ 的互斥锁
    std::condition_variable cond_;         // 线程同步条件变量
};

} // namespace reactor