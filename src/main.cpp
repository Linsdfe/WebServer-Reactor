/**
 * @file main.cpp
 * @brief 服务器入口文件：解析命令行参数、初始化配置、启动服务
 * 
 * 核心功能：
 * 1. 解析命令行参数（端口、IO线程数、MySQL连接信息）
 * 2. 自动计算IO线程数（CPU核心数*2）
 * 3. 打印启动信息，初始化并启动Server
 */
#include "server/server.h"
#include <iostream>
#include <thread>
#include <cstdlib>

/**
 * @brief 主函数：服务器入口
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return int 退出码（0表示正常退出）
 * 
 * 命令行参数规则：
 * ./server [端口] [IO线程数] [MySQL主机] [MySQL用户] [MySQL密码] [MySQL数据库]
 * - 端口：默认8888
 * - IO线程数：默认CPU核心数*2（≤0时自动计算）
 * - MySQL主机：默认localhost
 * - MySQL用户：默认root
 * - MySQL密码：默认空
 * - MySQL数据库：默认webserver
 */
int main(int argc, char* argv[]) {
    // 默认配置：端口8888，IO线程数自动计算，MySQL默认连接参数
    int port = 8888;
    int thread_num = 0;
    std::string mysql_host = "localhost";
    std::string mysql_user = "root";
    std::string mysql_password = "123456";
    std::string mysql_database = "webserver_db";

    // 解析命令行参数：优先使用用户输入
    if (argc >= 2) {
        port = atoi(argv[1]); // 端口参数（第2个参数）
    }
    if (argc >= 3) {
        thread_num = atoi(argv[2]); // IO线程数参数（第3个参数）
    }
    if (argc >= 4) {
        mysql_host = argv[3]; // MySQL主机参数（第4个参数）
    }
    if (argc >= 5) {
        mysql_user = argv[4]; // MySQL用户参数（第5个参数）
    }
    if (argc >= 6) {
        mysql_password = argv[5]; // MySQL密码参数（第6个参数）
    }
    if (argc >= 7) {
        mysql_database = argv[6]; // MySQL数据库参数（第7个参数）
    }

    // 自动计算IO线程数：CPU逻辑核心数*2（提升并发处理能力）
    unsigned int cpu_core_num = std::thread::hardware_concurrency();
    if (thread_num <= 0) {
        // 兜底：CPU核心数获取失败时默认8线程
        thread_num = (cpu_core_num == 0) ? 8 : static_cast<int>(cpu_core_num * 2);
    }

    // 打印启动信息（便于调试和日志查看）
    std::cout << "==========================================" << std::endl;
    std::cout << "  Reactor WebServer v1.0" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "  CPU Logical Cores: " << cpu_core_num << std::endl;
    std::cout << "  IO Threads:        " << thread_num << std::endl;
    std::cout << "  Listen Port:       " << port << std::endl;
    std::cout << "  MySQL Host:        " << mysql_host << std::endl;
    std::cout << "  MySQL User:        " << mysql_user << std::endl;
    std::cout << "  MySQL Database:    " << mysql_database << std::endl;
    std::cout << "==========================================" << std::endl;

    // 初始化并启动服务器（阻塞直到退出）
    reactor::Server server(port, thread_num, mysql_host, mysql_user, mysql_password, mysql_database);
    server.Start();

    return 0;
}