#include "net/epoller.h"
#include "net/channel.h"
#include <iostream>
#include <cstring>

namespace reactor {

Epoller::Epoller(int maxEvents) 
    : m_epollFd(epoll_create(1)), m_events(maxEvents) {
    if (m_epollFd < 0) {
        std::cerr << "[Error] Epoll create failed!" << std::endl;
        exit(EXIT_FAILURE);
    }
}

Epoller::~Epoller() {
    close(m_epollFd);
}

void Epoller::UpdateChannel(Channel* channel) {
    int fd = channel->Fd();
    uint32_t events = channel->Events();
    
    if (fd_to_channel_.find(fd) == fd_to_channel_.end()) {
        // 新增fd
        fd_to_channel_[fd] = channel;
        epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;
        ev.events = events;
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::cerr << "[Error] Epoll add fd=" << fd << " failed!" << std::endl;
        }
    } else {
        // 修改已有fd
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

void Epoller::RemoveChannel(Channel* channel) {
    int fd = channel->Fd();
    fd_to_channel_.erase(fd);
    epoll_event ev;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, &ev) < 0) {
        std::cerr << "[Error] Epoll del fd=" << fd << " failed!" << std::endl;
    }
}

void Epoller::Wait(int timeoutMs, std::vector<Channel*>& active_channels) {
    int num_events = epoll_wait(m_epollFd, m_events.data(), 
                                static_cast<int>(m_events.size()), timeoutMs);
    if (num_events > 0) {
        active_channels.reserve(num_events);
        for (int i = 0; i < num_events; ++i) {
            Channel* channel = fd_to_channel_[m_events[i].data.fd];
            channel->SetRevents(m_events[i].events);
            active_channels.push_back(channel);
        }
    }
}

} // namespace reactor
