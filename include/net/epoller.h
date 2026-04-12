#pragma once

/**
 * @file epoller.h
 * @brief Epoller 类：封装 epoll 系统调用，维护 fd 与 Channel 的映射，以及事件等待逻辑
 *
 * 【Reactor 架构位置】
 * - 是 **EventLoop 的核心组件**
 * - 每个 EventLoop 对应一个 Epoller
 * - 直接管理 epoll fd，负责事件的增删改查
 */

#include <sys/epoll.h>
#include <vector>
#include <unordered_map>
#include <unistd.h>

namespace reactor {

class Channel;

/**
 * @class Epoller
 * @brief Linux epoll 系统调用的封装类
 *
 * 核心职责：
 * 1. 创建/管理 epoll fd（epoll_create1）
 * 2. 增/删/改 Channel 监听事件（epoll_ctl：ADD/MOD/DEL）
 * 3. 调用 epoll_wait 获取就绪事件
 * 4. 维护 fd 到 Channel 的映射（fd_to_channel_），通过 fd 找回 Channel
 */
class Epoller {
public:
    /**
     * @brief Epoller 构造函数
     * @param maxEvents epoll 事件数组初始大小（默认 4096）
     *
     * 初始化流程：
     * 1. 创建 epoll fd（epoll_create1，EPOLL_CLOEXEC）
     * 2. 初始化事件数组（m_events）
     */
    explicit Epoller(int maxEvents = 4096);

    /**
     * @brief Epoller 析构函数：关闭 epoll fd
     */
    ~Epoller();

    /**
     * @brief 更新 Channel 到 epoll（新增或修改）
     * @param channel 待更新的 Channel
     *
     * 逻辑：
     * - fd 未注册：EPOLL_CTL_ADD（新增）
     * - fd 已注册：EPOLL_CTL_MOD（修改）
     *
     * 【线程安全】必须在所属 EventLoop 线程执行
     */
    void UpdateChannel(Channel* channel);

    /**
     * @brief 从 epoll 中移除 Channel
     * @param channel 待移除的 Channel
     *
     * 【线程安全】必须在所属 EventLoop 线程执行
     */
    void RemoveChannel(Channel* channel);

    /**
     * @brief 等待 epoll 事件，返回就绪 Channel 列表
     * @param timeoutMs 超时时间（毫秒）
     * @param active_channels 输出参数：就绪 Channel 列表
     *
     * 【线程安全】必须在所属 EventLoop 线程执行
     */
    void Wait(int timeoutMs, std::vector<Channel*>& active_channels);

private:
    int m_epollFd;                              // epoll 文件描述符
    std::vector<epoll_event> m_events;         // epoll 事件数组（存储就绪事件）
    std::unordered_map<int, Channel*> fd_to_channel_; // fd 到 Channel 的映射
};

} // namespace reactor