/**
 * @file eventloop.cpp
 * @brief EventLoop 事件循环核心实现：封装 epoll 事件循环、跨线程任务调度及唤醒机制
 *
 * 核心设计：
 * 1. One Loop Per Thread：每个线程绑定一个 EventLoop
 * 2. 跨线程任务调度：通过 RunInLoop/QueueInLoop 实现线程安全的任务提交
 * 3. 唤醒机制：使用 eventfd 实现线程间轻量级唤醒
 * 4. 性能优化：通过 swap 减少任务队列的锁持有时间
 */

#include "net/eventloop.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <iostream>

namespace reactor {

/**
 * @brief 创建 eventfd（跨线程唤醒专用）
 * @return int 初始化后的 eventfd（非阻塞+执行时关闭）
 *
 * eventfd 作用：
 * - 用于跨线程唤醒 epoll_wait，避免忙等待
 * - 写入 8 字节数据触发读事件，读取后清空缓冲区
 */
static int CreateEventFd() {
    int evtfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        std::cerr << "[Error] Failed to create eventfd" << std::endl;
        abort();
    }
    return evtfd;
}

/**
 * @brief EventLoop 构造函数
 *
 * 【关键修复】初始化列表顺序必须与头文件声明顺序完全一致
 * （C++ 标准规定：成员变量初始化顺序严格按照声明顺序，与初始化列表顺序无关）
 *
 * 初始化流程：
 * 1. 记录当前线程 ID（确保单线程单循环）
 * 2. 创建 Epoller（管理 epoll fd）
 * 3. 创建 eventfd（跨线程唤醒）
 * 4. 创建 Channel 管理 eventfd
 * 5. 注册 eventfd 读事件回调
 */
EventLoop::EventLoop()
    : thread_id_(std::this_thread::get_id()), // 1. 先初始化 thread_id_（声明顺序第一）
      epoller_(new Epoller()),                 // 2. 再初始化 epoller_
      looping_(false),                         // 3. 然后是 looping_
      quit_(false),                            // 4. 然后是 quit_
      wakeup_fd_(CreateEventFd()),             // 5. 后续顺序需与声明一致
      wakeup_channel_(new Channel(this, wakeup_fd_)),
      calling_pending_functors_(false) {
    // 注册 eventfd 读事件回调：唤醒时读取数据清空缓冲区
    wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
    // 开启 eventfd 读事件监听（EPOLLIN+EPOLLET）
    wakeup_channel_->EnableReading();
}

/**
 * @brief EventLoop 析构函数
 *
 * 清理流程：
 * 1. 禁用 eventfd 的所有事件
 * 2. 从 Epoller 移除 eventfd 的 Channel
 * 3. 关闭 eventfd
 */
EventLoop::~EventLoop() {
    wakeup_channel_->DisableAll();
    wakeup_channel_->Remove();
    close(wakeup_fd_);
}

/**
 * @brief 启动事件循环（阻塞运行，直到 Quit）
 *
 * 循环流程（Reactor 核心）：
 * 1. Epoll 等待就绪事件（timeout=100ms，平衡响应速度和 CPU 占用）
 * 2. 处理所有就绪 Channel 的事件（HandleEvent）
 * 3. 执行跨线程提交的任务（DoPendingFunctors）
 */
void EventLoop::Loop() {
    AssertInLoopThread(); // 确保在当前线程执行
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        std::vector<Channel*> active_channels;
        // 1. 等待 epoll 事件（timeout=100ms）
        epoller_->Wait(100, active_channels);

        // 2. 处理所有就绪事件（读/写/关闭/错误）
        for (Channel* channel : active_channels) {
            channel->HandleEvent();
        }

        // 3. 执行跨线程提交的任务（保证线程安全）
        DoPendingFunctors();
    }

    looping_ = false;
}

/**
 * @brief 退出事件循环
 *
 * 注意：
 * - 如果不在当前线程，需要唤醒 EventLoop（Wakeup）
 * - 唤醒后 epoll_wait 会立即返回，执行 DoPendingFunctors 后退出循环
 */
void EventLoop::Quit() {
    quit_ = true;
    // 非当前线程：唤醒 epoll_wait 确保及时退出
    if (!IsInLoopThread()) {
        Wakeup();
    }
}

/**
 * @brief 更新 Channel 的事件（增/改）
 * @param channel 待更新的 Channel
 *
 * 线程安全：必须在当前 EventLoop 线程执行
 */
void EventLoop::UpdateChannel(Channel* channel) {
    AssertInLoopThread();
    epoller_->UpdateChannel(channel);
}

/**
 * @brief 从 Epoller 中移除 Channel
 * @param channel 待移除的 Channel
 *
 * 线程安全：必须在当前 EventLoop 线程执行
 */
void EventLoop::RemoveChannel(Channel* channel) {
    AssertInLoopThread();
    epoller_->RemoveChannel(channel);
}

/**
 * @brief 在 EventLoop 线程执行任务
 * @param cb 任务函数
 *
 * 逻辑：
 * - 当前线程：立即执行
 * - 非当前线程：入队后唤醒
 */
void EventLoop::RunInLoop(Functor cb) {
    if (IsInLoopThread()) {
        cb();
    } else {
        QueueInLoop(std::move(cb));
    }
}

/**
 * @brief 将任务入队（跨线程调用）
 * @param cb 任务函数
 *
 * 线程安全：通过 mutex 保护任务队列
 * 唤醒条件：
 * - 非当前线程
 * - 当前线程正在执行任务队列（避免任务堆积）
 */
void EventLoop::QueueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.push_back(std::move(cb));
    }
    // 唤醒 epoll_wait 确保任务及时执行
    if (!IsInLoopThread() || calling_pending_functors_) {
        Wakeup();
    }
}

/**
 * @brief 唤醒 EventLoop（向 eventfd 写入 8 字节数据）
 *
 * eventfd 写入逻辑：
 * - 写入 uint64_t 类型的 1，触发读事件
 * - 非阻塞写入，即使缓冲区满也不阻塞（eventfd 缓冲区足够大）
 */
void EventLoop::Wakeup() {
    uint64_t one = 1;
    ssize_t n = write(wakeup_fd_, &one, sizeof(one));
    (void)n; // 忽略返回值，避免编译警告
}

/**
 * @brief 处理 eventfd 的读事件（读取数据清空缓冲区）
 *
 * 读取逻辑：
 * - 读取 uint64_t 类型的数据，清空 eventfd 缓冲区
 * - 非阻塞读取，避免无数据时阻塞
 */
void EventLoop::HandleRead() {
    uint64_t one = 1;
    ssize_t n = read(wakeup_fd_, &one, sizeof(one));
    (void)n;
}

/**
 * @brief 执行跨线程提交的任务
 *
 * 【性能优化】用 swap 减少锁持有时间：
 * - 先通过 swap 将任务队列交换到局部变量
 * - 再执行任务，此时 mutex 已释放，不阻塞其他线程入队
 */
void EventLoop::DoPendingFunctors() {
    std::vector<Functor> functors;
    calling_pending_functors_ = true;

    // 用 swap 减少锁持有时间（核心优化）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    // 执行所有任务（此时 mutex 已释放）
    for (const Functor& functor : functors) {
        functor();
    }

    calling_pending_functors_ = false;
}

/**
 * @brief 线程断言失败处理（打印日志并崩溃）
 *
 * 触发条件：
 * - 当前线程不是 EventLoop 创建时的线程
 * - 违反 One Loop Per Thread 设计原则
 */
void EventLoop::AbortNotInLoopThread() {
    std::cerr << "[Fatal] EventLoop is not in its creation thread!" << std::endl;
    abort();
}

} // namespace reactor