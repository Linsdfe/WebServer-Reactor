#pragma once

#include <sys/epoll.h>
#include <vector>
#include <unordered_map>
#include <unistd.h>

namespace reactor {

class Channel;

class Epoller {
public:
    explicit Epoller(int maxEvents = 4096);
    ~Epoller();

    void UpdateChannel(Channel* channel);
    void RemoveChannel(Channel* channel);
    void Wait(int timeoutMs, std::vector<Channel*>& active_channels);

private:
    int m_epollFd;
    std::vector<epoll_event> m_events;
    std::unordered_map<int, Channel*> fd_to_channel_;
};

} // namespace reactor
