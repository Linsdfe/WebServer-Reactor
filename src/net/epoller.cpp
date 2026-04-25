/**
 * @file epoller.cpp
 * @brief Epoller 类：封装 Linux epoll 系统调用，管理 IO 事件
 *
 * 核心功能：
 * 1. 创建/管理 epoll fd
 * 2. 增/删/改 Channel 监听事件（EPOLL_CTL_ADD/MOD/DEL）
 * 3. 调用 epoll_wait 获取就绪事件
 * 4. 维护 fd 到 Channel 的映射（fd_to_channel_）
 */

#include "net/epoller.h"
#include "net/channel.h"
#include <iostream>
#include <cstring>

namespace reactor {

/**
 * @brief Epoller 构造函数
 * @param maxEvents epoll 事件数组初始大小（默认 16）
 *
 * 初始化流程：
 * 1. 创建 epoll fd（epoll_create(1)，参数 1 已废弃但需保留）
 * 2. 初始化事件数组（m_events）
 */
Epoller::Epoller(int maxEvents)
    : m_epollFd(epoll_create(1)), m_events(maxEvents) {
    if (m_epollFd < 0) {
        std::cerr << "[Error] Epoll create failed!" << std::endl;
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Epoller 析构函数：关闭 epoll fd
 */
Epoller::~Epoller() {
    close(m_epollFd);
}

/**
 * @brief 更新 Channel 到 epoll（新增或修改）
 * @param channel 待更新的 Channel
 *
 * 逻辑：
 * - fd 未注册：EPOLL_CTL_ADD（新增）
 * - fd 已注册：EPOLL_CTL_MOD（修改）
 *
 * 注意：
 * - 使用 ev.data.fd 存储 fd，通过 fd_to_channel_ 映射回 Channel
 * - 事件类型通过 channel->Events() 获取（EPOLLIN/EPOLLOUT/EPOLLET 等）
 */
void Epoller::UpdateChannel(Channel* channel) {
    int fd = channel->Fd();
    uint32_t events = channel->Events();
    
    if (fd_to_channel_.find(fd) == fd_to_channel_.end()) {
        // ========== 新增 fd（EPOLL_CTL_ADD） ==========
        fd_to_channel_[fd] = channel;
        epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;          // 存储 fd，用于后续映射
        ev.events = events;        // 存储感兴趣事件
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::cerr << "[Error] Epoll add fd=" << fd << " failed!" << std::endl;
        }
    } else {
        // ========== 修改已有 fd（EPOLL_CTL_MOD） ==========
        fd_to_channel_[fd] = channel;
        epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;
        ev.events = events;
        if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            std::cerr << "[Error] Epoll mod fd=" << fd << " failed!" << std::endl;
        }
    }
}

/**
 * @brief 从 epoll 中移除 Channel
 * @param channel 待移除的 Channel
 *
 * 逻辑：
 * 1. 从 fd_to_channel_ 中删除映射
 * 2. 调用 epoll_ctl(EPOLL_CTL_DEL) 移除 fd
 *
 * 注意：
 * - EPOLL_CTL_DEL 时 ev 参数可以为空（但需传递有效指针）
 */
void Epoller::RemoveChannel(Channel* channel) {
    int fd = channel->Fd();
    fd_to_channel_.erase(fd); // 删除映射
    epoll_event ev;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, &ev) < 0) {
        std::cerr << "[Error] Epoll del fd=" << fd << " failed!" << std::endl;
    }
}

/**
 * @brief 等待 epoll 事件，返回就绪 Channel 列表
 * @param timeoutMs 超时时间（毫秒）
 * @param active_channels 输出参数：就绪 Channel 列表
 *
 * 流程：
 * 1. 调用 epoll_wait 等待事件
 * 2. 遍历就绪事件，通过 fd_to_channel_ 映射回 Channel
 * 3. 设置 Channel 的实际触发事件（SetRevents）
 * 4. 将 Channel 加入 active_channels
 *
 * 注意：
 * - active_channels 会先 reserve 空间，避免频繁重新分配
 * - num_events=0 表示超时，无就绪事件
 */
void Epoller::Wait(int timeoutMs, std::vector<Channel*>& active_channels) {
    int num_events = epoll_wait(m_epollFd, m_events.data(), 
                                static_cast<int>(m_events.size()), timeoutMs);
    if (num_events > 0) {
        active_channels.reserve(num_events);
        for (int i = 0; i < num_events; ++i) {
            auto it = fd_to_channel_.find(m_events[i].data.fd);
            if (it == fd_to_channel_.end()) {
                continue;
            }
            Channel* channel = it->second;
            channel->SetRevents(m_events[i].events);
            active_channels.push_back(channel);
        }
    }
}

} // namespace reactor