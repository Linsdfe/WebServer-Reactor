#pragma once

/**
 * @file server.h
 * @brief 服务器核心类：主 Reactor 管理监听与连接分发，从 Reactor 处理 IO
 *
 * Server 类是整个 Reactor 架构的入口，负责：
 * 1. 初始化主 EventLoop、IO 线程池、Acceptor
 * 2. 管理 TCP 连接生命周期（创建/销毁）
 * 3. 线程安全的连接映射管理
 * 4. 初始化 MySQL/Redis 连接池及主从配置
 */

#include "net/eventloop.h"
#include "net/eventloopthreadpool.h"
#include "net/acceptor.h"
#include "server/tcpconnection.h"
#include "server/cachemanager.h"
#include "server/redis_cache.h"
#include "auth/mysql_connection_pool.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace reactor {

/**
 * @class Server
 * @brief Reactor 架构入口类，协调所有模块运行
 *
 * 架构角色：
 * - 主 Reactor：通过 Acceptor 监听端口，接受新连接
 * - 从 Reactor：通过 EventLoopThreadPool 管理 IO 线程
 * - 连接管理：维护 fd → TcpConnection 映射
 */
class Server {
public:
    Server(int port, int thread_num = 8,
           const std::string& mysql_host = "localhost",
           const std::string& mysql_user = "root",
           const std::string& mysql_password = "",
           const std::string& mysql_database = "webserver",
           const std::vector<MySQLNodeConfig>& mysql_slaves = {},
           const std::vector<RedisNodeConfig>& redis_slaves = {});

    ~Server();

    void Start();

private:
    void NewConnection(int sockfd, const struct sockaddr_in& addr);
    void RemoveConnection(int sockfd);
    void RemoveConnectionInLoop(int sockfd);

    std::unique_ptr<EventLoop> base_loop_;
    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    std::string src_dir_;
    int port_;
    std::string mysql_host_;
    std::string mysql_user_;
    std::string mysql_password_;
    std::string mysql_database_;
    std::unique_ptr<CacheManager> cache_manager_;
    std::vector<MySQLNodeConfig> mysql_slaves_;
    std::vector<RedisNodeConfig> redis_slaves_;
};

} // namespace reactor
