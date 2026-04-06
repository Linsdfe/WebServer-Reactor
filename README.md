# WebServer-Reactor
基于 C++11 实现的**主从Reactor模式**高并发Web服务器，完全工程化结构设计，支持HTTP/1.1基础协议、长连接、静态资源服务。

## 核心特性
- ✅ 标准Reactor架构：主Reactor负责连接接受，从Reactor负责IO事件处理
- ✅ One Loop Per Thread：每个IO线程独立EventLoop，无锁竞争
- ✅ 高性能IO：Epoll边缘触发(ET) + 非阻塞Socket + TCP_NODELAY
- ✅ 工程化结构：模块化分层设计、CMake构建、统一命名空间
- ✅ HTTP协议支持：GET请求、长连接(Keep-Alive)、粘包处理、静态资源响应

## 环境要求
- 操作系统：Linux 内核2.6+（依赖Epoll系统调用）
- 编译器：GCC 4.8+ / Clang 3.3+（支持C++11）
- 构建工具：CMake 3.10+

## 快速开始
### 1. 编译项目
```bash
# 克隆项目并进入根目录
git clone <仓库地址>
cd WebServer-Reactor

# 创建编译目录并生成构建文件
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# 并行编译（-j后接CPU核心数）
make -j$(nproc)
