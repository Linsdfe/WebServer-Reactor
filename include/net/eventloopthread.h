#pragma once

#include "net/eventloop.h"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace reactor {

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    // 启动线程并返回EventLoop指针
    EventLoop* StartLoop();

private:
    void ThreadFunc();

    EventLoop* loop_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
};

} // namespace reactor
