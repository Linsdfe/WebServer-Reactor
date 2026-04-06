#pragma once

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
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void Loop();
    void Quit();

    void UpdateChannel(Channel* channel);
    void RemoveChannel(Channel* channel);

    void RunInLoop(Functor cb);
    void QueueInLoop(Functor cb);

    void AssertInLoopThread() {
        if (!IsInLoopThread()) AbortNotInLoopThread();
    }
    bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

private:
    void AbortNotInLoopThread();
    void DoPendingFunctors();
    void Wakeup();
    void HandleRead();

    // 【关键修复】成员变量声明顺序必须与构造函数初始化列表完全一致
    // 顺序：1. thread_id_ 2. epoller_ 3. looping_ 4. quit_ ...
    const std::thread::id thread_id_;
    std::unique_ptr<Epoller> epoller_;
    bool looping_;
    bool quit_;

    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;
    std::mutex mutex_;
    std::vector<Functor> pending_functors_;
    bool calling_pending_functors_;
};

} // namespace reactor
