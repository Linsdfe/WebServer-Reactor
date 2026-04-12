#pragma once

/**
 * @file eventloop.h
 * @brief EventLoop类：事件循环核心（One Loop Per Thread）
 * 
 * EventLoop核心职责：
 * 1. 运行事件循环（Loop）：Epoll等待→处理就绪事件→执行待处理任务
 * 2. 管理Channel（增删改）：转发到Epoller
 * 3. 跨线程任务调度（RunInLoop/QueueInLoop）：通过eventfd唤醒
 * 4. 线程安全：保证每个EventLoop只在所属线程运行
 */
#include "net/epoller.h"
#include "net/channel.h"
#include <thread>
#include <mutex>
#include <functional>
#include <vector>
#include <memory>

namespace reactor {

class EventLoop {
public:
    /**
     * @brief 任务函数类型（跨线程执行）
     */
    using Functor = std::function<void()>;

    /**
     * @brief 构造函数：初始化Epoller、eventfd、wakeup_channel
     * 
     * 【关键】成员变量初始化顺序必须与声明顺序一致（避免未定义行为）
     */
    EventLoop();
    
    /**
     * @brief 析构函数：清理wakeup_channel、关闭eventfd
     */
    ~EventLoop();

    /**
     * @brief 启动事件循环（阻塞，直到Quit）
     * 
     * 循环流程：
     * 1. Epoll等待就绪事件（timeout=100ms）
     * 2. 处理所有就绪Channel的事件（HandleEvent）
     * 3. 执行跨线程提交的任务（DoPendingFunctors）
     */
    void Loop();
    
    /**
     * @brief 退出事件循环
     * 
     * 注意：如果不在当前线程，需唤醒EventLoop（Wakeup）
     */
    void Quit();

    /**
     * @brief 更新Channel的事件（转发到Epoller）
     * @param channel 待更新的Channel
     */
    void UpdateChannel(Channel* channel);
    
    /**
     * @brief 移除Channel（转发到Epoller）
     * @param channel 待移除的Channel
     */
    void RemoveChannel(Channel* channel);

    /**
     * @brief 在EventLoop线程执行任务（当前线程则立即执行，否则入队）
     * @param cb 任务函数
     */
    void RunInLoop(Functor cb);
    
    /**
     * @brief 将任务入队（跨线程调用）
     * @param cb 任务函数
     * 
     * 注意：入队后唤醒EventLoop（确保任务被及时执行）
     */
    void QueueInLoop(Functor cb);

    /**
     * @brief 断言：必须在EventLoop所属线程执行
     * 
     * 违反则触发AbortNotInLoopThread（崩溃并打印日志）
     */
    void AssertInLoopThread() {
        if (!IsInLoopThread()) AbortNotInLoopThread();
    }
    
    /**
     * @brief 判断当前线程是否是EventLoop所属线程
     * @return bool true=是，false=否
     */
    bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

private:
    /**
     * @brief 断言失败处理：打印日志并崩溃
     */
    void AbortNotInLoopThread();
    
    /**
     * @brief 执行待处理任务（PendingFunctors）
     * 
     * 优化：通过swap减少锁持有时间，避免阻塞生产者
     */
    void DoPendingFunctors();
    
    /**
     * @brief 唤醒EventLoop（向eventfd写入数据）
     */
    void Wakeup();
    
    /**
     * @brief 处理wakeup_fd_的读事件（清空eventfd）
     */
    void HandleRead();

    // ========== 成员变量（【关键】声明顺序=初始化顺序） ==========
    const std::thread::id thread_id_;       // 所属线程ID（创建时赋值）
    std::unique_ptr<Epoller> epoller_;      // Epoller对象（管理epoll fd）
    bool looping_;                          // 是否正在运行事件循环
    bool quit_;                             // 是否退出循环

    int wakeup_fd_;                         // eventfd（跨线程唤醒）
    std::unique_ptr<Channel> wakeup_channel_;// 管理wakeup_fd_的Channel
    std::mutex mutex_;                      // 保护pending_functors_的互斥锁
    std::vector<Functor> pending_functors_; // 待执行的跨线程任务
    bool calling_pending_functors_;         // 是否正在执行待处理任务
};

} // namespace reactor