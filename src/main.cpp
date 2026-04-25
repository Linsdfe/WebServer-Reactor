/**
 * @file main.cpp
 * @brief 服务器入口文件：解析命令行参数、初始化配置、启动服务
 * 
 * 核心功能：
 * 1. 解析命令行参数（端口、IO线程数、MySQL连接信息、MySQL从库配置、Redis从库配置）
 * 2. 自动计算IO线程数（CPU核心数*2）
 * 3. 打印启动信息，初始化并启动Server
 */
#include "server/server.h"
#include "auth/mysql_connection_pool.h"
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>
 
/**
 * @brief 主函数：服务器入口
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return int 退出码（0表示正常退出）
 * 
 * 命令行参数规则：
 * ./server [端口] [IO线程数] [MySQL主机] [MySQL用户] [MySQL密码] [MySQL数据库] [MySQL从库列表] [Redis从库列表]
 * - 端口：默认8888
 * - IO线程数：默认CPU核心数*2（≤0时自动计算）
 * - MySQL主机：默认localhost
 * - MySQL用户：默认root
 * - MySQL密码：默认空
 * - MySQL数据库：默认webserver
 * - MySQL从库列表：格式 host1:port1,host2:port2（默认空，表示单机模式）
 * - Redis从库列表：格式 host1:port1,host2:port2（默认空，表示单机模式）
 * 
 * 示例：
 * ./server 8888 8 localhost root 123456 webserver_db 127.0.0.1:3307,127.0.0.1:3308 127.0.0.1:6380,127.0.0.1:6381
 */
int main(int argc, char* argv[]) {
    int port = 8888;
    int thread_num = 0;
    std::string mysql_host = "localhost";
    std::string mysql_user = "root";
    std::string mysql_password = "123456";
    std::string mysql_database = "webserver_db";
    std::vector<reactor::MySQLNodeConfig> mysql_slaves;
    std::vector<reactor::RedisNodeConfig> redis_slaves;

    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    if (argc >= 3) {
        thread_num = atoi(argv[2]);
    }
    if (argc >= 4) {
        mysql_host = argv[3];
    }
    if (argc >= 5) {
        mysql_user = argv[4];
    }
    if (argc >= 6) {
        mysql_password = argv[5];
    }
    if (argc >= 7) {
        mysql_database = argv[6];
    }
    if (argc >= 8) {
        std::string mysql_slaves_str = argv[7];
        if (!mysql_slaves_str.empty()) {
            std::string token;
            size_t start = 0;
            size_t end = mysql_slaves_str.find(',');
            while (end != std::string::npos) {
                token = mysql_slaves_str.substr(start, end - start);
                size_t colon = token.find(':');
                if (colon != std::string::npos) {
                    reactor::MySQLNodeConfig config;
                    config.host = token.substr(0, colon);
                    config.port = std::atoi(token.substr(colon + 1).c_str());
                    config.user = mysql_user;
                    config.password = mysql_password;
                    config.database = mysql_database;
                    config.pool_size = 0;
                    config.is_master = false;
                    mysql_slaves.push_back(config);
                }
                start = end + 1;
                end = mysql_slaves_str.find(',', start);
            }
            token = mysql_slaves_str.substr(start);
            size_t colon = token.find(':');
            if (colon != std::string::npos) {
                reactor::MySQLNodeConfig config;
                config.host = token.substr(0, colon);
                config.port = std::atoi(token.substr(colon + 1).c_str());
                config.user = mysql_user;
                config.password = mysql_password;
                config.database = mysql_database;
                config.pool_size = 0;
                config.is_master = false;
                mysql_slaves.push_back(config);
            }
        }
    }
    if (argc >= 9) {
        std::string redis_slaves_str = argv[8];
        if (!redis_slaves_str.empty()) {
            std::string token;
            size_t start = 0;
            size_t end = redis_slaves_str.find(',');
            while (end != std::string::npos) {
                token = redis_slaves_str.substr(start, end - start);
                size_t colon = token.find(':');
                if (colon != std::string::npos) {
                    redis_slaves.push_back(reactor::RedisNodeConfig(
                        token.substr(0, colon), std::atoi(token.substr(colon + 1).c_str())
                    ));
                }
                start = end + 1;
                end = redis_slaves_str.find(',', start);
            }
            token = redis_slaves_str.substr(start);
            size_t colon = token.find(':');
            if (colon != std::string::npos) {
                redis_slaves.push_back(reactor::RedisNodeConfig(
                    token.substr(0, colon), std::atoi(token.substr(colon + 1).c_str())
                ));
            }
        }
    }

    unsigned int cpu_core_num = std::thread::hardware_concurrency();
    if (thread_num <= 0) {
        thread_num = (cpu_core_num == 0) ? 8 : static_cast<int>(cpu_core_num * 2);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  WebServer-Reactor Starting..." << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Port: " << port << std::endl;
    std::cout << "  IO Threads: " << thread_num << std::endl;
    std::cout << "  MySQL: " << mysql_host << "/" << mysql_database << std::endl;
    std::cout << "  MySQL Slaves: " << mysql_slaves.size() << std::endl;
    std::cout << "  Redis Slaves: " << redis_slaves.size() << std::endl;
    std::cout << "========================================" << std::endl;

    reactor::Server server(port, thread_num, mysql_host, mysql_user, mysql_password, mysql_database, mysql_slaves, redis_slaves);
    server.Start();

    return 0;
}
