#include "server/server.h"
#include <iostream>
#include <thread>
#include <cstdlib>

int main(int argc, char* argv[]) {
    // 默认参数
    int port = 8888;
    int thread_num = 0;

    // 支持命令行参数：./server [端口] [IO线程数]
    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    if (argc >= 3) {
        thread_num = atoi(argv[2]);
    }

    // 自动计算IO线程数：CPU核心数*2
    unsigned int cpu_core_num = std::thread::hardware_concurrency();
    if (thread_num <= 0) {
        thread_num = (cpu_core_num == 0) ? 8 : static_cast<int>(cpu_core_num * 2);
    }

    // 打印启动信息
    std::cout << "==========================================" << std::endl;
    std::cout << "  Reactor WebServer v1.0" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "  CPU Logical Cores: " << cpu_core_num << std::endl;
    std::cout << "  IO Threads:        " << thread_num << std::endl;
    std::cout << "  Listen Port:       " << port << std::endl;
    std::cout << "==========================================" << std::endl;

    // 启动服务器
    reactor::Server server(port, thread_num);
    server.Start();

    return 0;
}
