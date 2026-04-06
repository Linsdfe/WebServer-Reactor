#pragma once

#include "net/eventloop.h"
#include "net/eventloopthread.h"
#include <vector>
#include <memory>

namespace reactor {

class EventLoopThreadPool {
public:
    EventLoopThreadPool(EventLoop* base_loop, int thread_num);
    ~EventLoopThreadPool();

    // 启动线程池
    void Start();
    // 轮询获取下一个IO线程的EventLoop
    EventLoop* GetNextLoop();

private:
    EventLoop* base_loop_; // 主Reactor的EventLoop
    bool started_;
    int thread_num_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
    int next_; // 轮询索引
};

} // namespace reactor
