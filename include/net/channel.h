#pragma once

/**
 * @file channel.h
 * @brief Channel 类：表示感兴趣事件与 fd 之间的绑定，负责事件到回调的分发
 *
 * 【Reactor 架构位置】
 * - 是 **EventLoop 与 fd 之间的桥梁**
 * - 每个 fd 对应一个 Channel（监听 fd、连接 fd、eventfd 等）
 * - 不拥有 fd，仅负责事件注册、回调触发
 */

#include <functional>
#include <sys/epoll.h>

namespace reactor {

class EventLoop;

/**
 * @class Channel
 * @brief fd 与事件的绑定封装类
 *
 * 核心职责：
 * 1. 管理单个 fd 的**感兴趣事件（events_）**和**实际触发事件（revents_）**
 * 2. 注册事件回调（读/写/关闭/错误）
 * 3. 事件处理入口（HandleEvent）：根据 revents_ 触发对应回调
 * 4. 事件控制（EnableReading/DisableAll 等）：通过 Update 同步到 Epoller
 */
class Channel {
public:
    /**
     * @brief 事件回调类型（无参数、无返回值）
     */
    using EventCallback = std::function<void()>;

    /**
     * @brief Channel 构造函数
     * @param loop 所属的 EventLoop
     * @param fd 关联的文件描述符（socket fd/eventfd 等）
     */
    Channel(EventLoop* loop, int fd);

    /**
     * @brief Channel 析构函数（空实现，不拥有 fd）
     */
    ~Channel();

    // ========== 事件回调设置 ==========
    /**
     * @brief 设置读事件回调（EPOLLIN 触发）
     */
    void SetReadCallback(EventCallback cb) { read_callback_ = std::move(cb); }
    /**
     * @brief 设置写事件回调（EPOLLOUT 触发）
     */
    void SetWriteCallback(EventCallback cb) { write_callback_ = std::move(cb); }
    /**
     * @brief 设置关闭事件回调（EPOLLHUP/EPOLLRDHUP 触发）
     */
    void SetCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }
    /**
     * @brief 设置错误事件回调（EPOLLERR 触发）
     */
    void SetErrorCallback(EventCallback cb) { error_callback_ = std::move(cb); }

    /**
     * @brief 事件处理入口（Epoller 回调触发）
     *
     * 事件处理优先级：
     * 1. 错误事件（EPOLLERR）→ error_callback_
     * 2. 关闭事件（EPOLLHUP/EPOLLRDHUP）→ close_callback_
     * 3. 可读事件（EPOLLIN）→ read_callback_
     * 4. 可写事件（EPOLLOUT）→ write_callback_
     */
    void HandleEvent();

    // ========== 基础属性获取 ==========
    /**
     * @brief 获取关联的 fd
     */
    int Fd() const { return fd_; }
    /**
     * @brief 获取感兴趣的事件（events_）
     */
    uint32_t Events() const { return events_; }
    /**
     * @brief 设置实际触发的事件（revents_，Epoller 调用）
     */
    void SetRevents(uint32_t revt) { revents_ = revt; }

    // ========== 事件控制 ==========
    /**
     * @brief 开启读事件（EPOLLIN + EPOLLET）
     */
    void EnableReading() { events_ |= EPOLLIN | EPOLLET; Update(); }
    /**
     * @brief 开启写事件（EPOLLOUT + EPOLLET）
     */
    void EnableWriting() { events_ |= EPOLLOUT | EPOLLET; Update(); }
    /**
     * @brief 关闭写事件（保留其他事件）
     */
    void DisableWriting() { events_ &= ~EPOLLOUT; Update(); }
    /**
     * @brief 禁用所有事件
     */
    void DisableAll() { events_ = 0; Update(); }
    /**
     * @brief 判断是否开启写事件
     */
    bool IsWriting() const { return events_ & EPOLLOUT; }

    /**
     * @brief 从 EventLoop 中移除当前 Channel
     *
     * 【线程安全】必须在所属 EventLoop 线程执行
     */
    void Remove();

private:
    /**
     * @brief 更新事件到 Epoller（封装 EventLoop::UpdateChannel）
     */
    void Update();

    EventLoop* loop_;                      // 所属的 EventLoop
    const int fd_;                         // 关联的文件描述符（不拥有）
    uint32_t events_;                      // 感兴趣的事件（EPOLLIN/EPOLLOUT/EPOLLET 等）
    uint32_t revents_;                     // 实际触发的事件（Epoller 设置）

    EventCallback read_callback_;          // 读事件回调
    EventCallback write_callback_;         // 写事件回调
    EventCallback close_callback_;         // 关闭事件回调
    EventCallback error_callback_;         // 错误事件回调
};

} // namespace reactor