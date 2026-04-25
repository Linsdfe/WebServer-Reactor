/**
 * @file server.cpp
 * @brief 服务器核心类实现：主Reactor管理监听、连接分发，从Reactor处理IO
 * 
 * Server类是整个Reactor架构的入口，负责：
 * 1. 初始化主EventLoop、IO线程池、Acceptor
 * 2. 管理TCP连接生命周期（创建/销毁）
 * 3. 线程安全的连接映射管理
 */
#include "server/server.h"
#include "auth/mysql_connection_pool.h"
#include "monitor/metrics_collector.h"
#include <unistd.h>
#include <iostream>

namespace reactor {

/**
 * @brief Server构造函数
 * @param port 监听端口
 * @param thread_num IO线程数（最终传给EventLoopThreadPool）
 * @param mysql_host MySQL主机地址
 * @param mysql_user MySQL用户名
 * @param mysql_password MySQL密码
 * @param mysql_database MySQL数据库名
 * @param mysql_slaves MySQL从库配置列表
 * @param redis_slaves Redis从库配置列表
 * 
 * 初始化流程：
 * 1. 创建主Reactor（base_loop_）
 * 2. 创建IO线程池（thread_pool_）
 * 3. 创建Acceptor（负责监听端口、接受新连接）
 * 4. 定位静态资源目录（优先可执行文件目录，兜底当前目录）
 * 5. 设置新连接回调函数
 * 6. 初始化MySQL和Redis连接参数
 */
Server::Server(int port, int thread_num, 
               const std::string& mysql_host, 
               const std::string& mysql_user, 
               const std::string& mysql_password, 
               const std::string& mysql_database,
               const std::vector<MySQLNodeConfig>& mysql_slaves,
               const std::vector<RedisNodeConfig>& redis_slaves)
    : base_loop_(new EventLoop()),
      thread_pool_(new EventLoopThreadPool(base_loop_.get(), thread_num)),
      acceptor_(new Acceptor(base_loop_.get(), port)),
      port_(port),
      mysql_host_(mysql_host),
      mysql_user_(mysql_user),
      mysql_password_(mysql_password),
      mysql_database_(mysql_database),
      mysql_slaves_(mysql_slaves),
      redis_slaves_(redis_slaves) {
    
    // ========== 定位静态资源目录（www） ==========
    char path[256];
    // 读取当前进程可执行文件的绝对路径（/proc/self/exe是符号链接）
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (len != -1) {
        path[len] = '\0';
        std::string exe_path(path);
        // 截取可执行文件所在目录（去掉文件名）
        size_t pos = exe_path.find_last_of('/');
        src_dir_ = exe_path.substr(0, pos + 1) + "www/";
    } else {
        // 兜底策略：当前工作目录下的www
        src_dir_ = "./www/";
    }

    // ========== 初始化缓存管理器 ==========
    cache_manager_ = std::make_unique<CacheManager>();

    // ========== 设置新连接回调 ==========
    // 绑定NewConnection到Acceptor的回调，Acceptor接受连接后触发
    acceptor_->SetNewConnectionCallback(
        std::bind(&Server::NewConnection, this, std::placeholders::_1, std::placeholders::_2)
    );
}

/**
 * @brief Server析构函数（空实现，智能指针自动释放资源）
 */
Server::~Server() {}

/**
 * @brief 启动服务器核心流程
 * 
 * 启动步骤：
 * 1. 启动IO线程池（创建并启动所有从Reactor线程）
 * 2. 主Reactor中启动Acceptor的监听（注册listenfd到epoll）
 * 3. 启动主Reactor的事件循环（base_loop_->Loop()）
 */
void Server::Start() {
    int cpu_cores = std::thread::hardware_concurrency();
    int io_threads = thread_pool_->GetThreadNum();
    
    int mysql_pool_size = io_threads * 2;
    int redis_pool_size = io_threads * 2;
    
    std::cout << "==========================================" << std::endl;
    std::cout << "   Reactor WebServer v1.0" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "   CPU Logical Cores: " << cpu_cores << std::endl;
    std::cout << "   IO Threads:        " << io_threads << std::endl;
    std::cout << "   Listen Port:      " << port_ << std::endl;
    std::cout << "   MySQL Host:       " << mysql_host_ << std::endl;
    std::cout << "   MySQL User:       " << mysql_user_ << std::endl;
    std::cout << "   MySQL Database:   " << mysql_database_ << std::endl;
    std::cout << "   MySQL Pool Size:  " << mysql_pool_size << std::endl;
    std::cout << "   Redis Host:       localhost" << std::endl;
    std::cout << "   Redis Port:       6379" << std::endl;
    std::cout << "   Redis Pool Size:  " << redis_pool_size << std::endl;
    if (!redis_slaves_.empty()) {
        std::cout << "   Redis Slaves:     " << redis_slaves_.size() << std::endl;
        for (size_t i = 0; i < redis_slaves_.size(); ++i) {
            std::cout << "     Slave-" << i << ": " << redis_slaves_[i].host << ":" << redis_slaves_[i].port << std::endl;
        }
    }
    std::cout << "==========================================" << std::endl;
    
    std::cout << "[Info] Resource directory: " << src_dir_ << std::endl;
    
    std::cout << "[Info] Initializing MySQL connection pool..." << std::endl;
    if (mysql_slaves_.empty()) {
        MySQLConnectionPool::GetInstance().Initialize(
            mysql_host_, mysql_user_, mysql_password_, mysql_database_, 3306, mysql_pool_size
        );
    } else {
        MySQLNodeConfig master_config;
        master_config.host = mysql_host_;
        master_config.port = 3306;
        master_config.user = mysql_user_;
        master_config.password = mysql_password_;
        master_config.database = mysql_database_;
        master_config.pool_size = mysql_pool_size;
        
        MySQLConnectionPool::GetInstance().InitializeWithSlaves(master_config, mysql_slaves_);
        MySQLConnectionPool::GetInstance().EnableSemiSync(3000);
        std::cout << "[Info] MySQL master-slave replication enabled with " << mysql_slaves_.size() << " slaves" << std::endl;
    }

    MySQLConnectionPool::GetInstance().SetReplicationLagAlert(5000);
    MySQLConnectionPool::GetInstance().SetFailoverCallback(
        [](const std::string& old_master, const std::string& new_master) {
            std::cerr << "[MySQL-FAILOVER] Master changed: " << old_master << " -> " << new_master << std::endl;
            MetricsCollector::Instance().IncMySQLFailovers();
        }
    );
    MySQLConnectionPool::GetInstance().SetHealthAlertCallback(
        [](const std::string& node, bool is_healthy) {
            if (!is_healthy) {
                std::cerr << "[MySQL-ALERT] Node " << node << " is unhealthy!" << std::endl;
            }
        }
    );
    MySQLConnectionPool::GetInstance().StartHealthCheck(10);
    
    std::cout << "[Info] Initializing Redis connection pool..." << std::endl;
    if (redis_slaves_.empty()) {
        RedisCache::GetInstance().Initialize("localhost", 6379, "", 0, redis_pool_size);
    } else {
        RedisNodeConfig master_config("localhost", 6379, "", 0, redis_pool_size);
        RedisCache::GetInstance().InitializeWithSlaves(master_config, redis_slaves_);
    }

    RedisCache::GetInstance().SetFailoverCallback(
        [](const std::string& old_master, const std::string& new_master) {
            std::cerr << "[Redis-FAILOVER] Master changed: " << old_master << " -> " << new_master << std::endl;
        }
    );
    RedisCache::GetInstance().SetHealthAlertCallback(
        [](const std::string& node, bool is_healthy) {
            if (!is_healthy) {
                std::cerr << "[Redis-ALERT] Node " << node << " is unhealthy!" << std::endl;
            }
        }
    );
    RedisCache::GetInstance().StartHealthCheck(10);
    
    std::cout << "[Info] Cache manager initialized" << std::endl;
    std::cout << "[Info] Metrics endpoint: http://localhost:" << port_ << "/metrics" << std::endl;
    std::cout << "[Info] Server start success!" << std::endl;
    // 启动IO线程池：创建thread_num个EventLoopThread并启动
    thread_pool_->Start();
    // 主Reactor线程中执行Acceptor::Listen（保证线程安全）
    base_loop_->RunInLoop(std::bind(&Acceptor::Listen, acceptor_.get()));
    // 启动主Reactor的事件循环（阻塞，直到退出）
    base_loop_->Loop();
}

/**
 * @brief 处理新连接（Acceptor回调触发）
 * @param sockfd 新连接的socket fd
 * @param addr 客户端地址（当前未使用，保留扩展）
 * 
 * 核心逻辑：
 * 1. 断言：必须在主Reactor线程执行（Acceptor的回调在主Reactor）
 * 2. 轮询获取一个从Reactor的EventLoop（负载均衡）
 * 3. 创建TcpConnection对象，管理新连接
 * 4. 设置连接关闭回调，关联到Server::RemoveConnection
 * 5. 在从Reactor线程中初始化连接（注册fd到epoll）
 */
void Server::NewConnection(int sockfd, const struct sockaddr_in& addr) {
    (void)addr;
    base_loop_->AssertInLoopThread();

    MetricsCollector::Instance().IncTotalConnections();

    EventLoop* io_loop = thread_pool_->GetNextLoop();

    // 创建TCP连接对象：托管到智能指针，保证生命周期
    std::shared_ptr<TcpConnection> conn = std::make_shared<TcpConnection>(io_loop, sockfd, src_dir_, 
                                                                         cache_manager_.get());
    // 保存连接到映射表，管理所有活跃连接
    connections_[sockfd] = conn;

    // 设置连接关闭回调：连接关闭时通知Server清理
    conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

    // 在IO线程中初始化连接（注册fd到epoll，开启读事件）
    io_loop->RunInLoop(std::bind(&TcpConnection::ConnectEstablished, conn));
}

/**
 * @brief 触发连接移除（TcpConnection回调触发）
 * @param sockfd 要关闭的连接fd
 * 
 * 注意：
 * 1. 该函数可能在从Reactor线程调用，需转发到主Reactor线程执行
 * 2. 保证连接管理的线程安全（所有连接操作在主Reactor）
 */
void Server::RemoveConnection(int sockfd) {
    // 转发到主Reactor线程执行，避免多线程竞争connections_
    base_loop_->RunInLoop(std::bind(&Server::RemoveConnectionInLoop, this, sockfd));
}

/**
 * @brief 实际执行连接移除（主Reactor线程）
 * @param sockfd 要关闭的连接fd
 * 
 * 核心逻辑：
 * 1. 断言：必须在主Reactor线程执行
 * 2. 从映射表中移除连接，获取智能指针（延长生命周期）
 * 3. 在从Reactor线程中销毁连接（注销epoll、关闭fd）
 */
void Server::RemoveConnectionInLoop(int sockfd) {
    base_loop_->AssertInLoopThread();
    auto it = connections_.find(sockfd);
    if (it != connections_.end()) {
        std::shared_ptr<TcpConnection> conn = it->second;
        connections_.erase(it);

        MetricsCollector::Instance().DecTotalConnections();

        EventLoop* io_loop = conn->GetLoop();
        io_loop->RunInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
    }
}

} // namespace reactor