/**
 * @file tcpconnection.cpp
 * @brief TcpConnection类实现：处理单个TCP连接的IO和HTTP解析/响应
 * 
 * 核心流程：
 * 1. 读事件：读取数据→解析HTTP请求→构建响应→触发写事件
 * 2. 写事件：发送响应数据→处理长/短连接
 * 3. 关闭事件：通知Server清理连接
 * 4. 错误事件：触发关闭逻辑
 */
#include "server/tcpconnection.h"
#include <unistd.h>
#include <errno.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/socket.h> // 【修复】添加recv/send定义头文件
#include <sys/types.h>

namespace reactor {

/**
 * @brief TcpConnection构造函数
 * @param loop 所属的从Reactor
 * @param fd 连接fd
 * @param src_dir 静态资源目录
 * 
 * 初始化流程：
 * 1. 保存基本参数（loop/fd/src_dir）
 * 2. 创建Channel管理连接fd
 * 3. 注册所有事件回调（读/写/关闭/错误）
 */
TcpConnection::TcpConnection(EventLoop* loop, int fd, const std::string& src_dir)
    : loop_(loop), channel_(new Channel(loop, fd)), fd_(fd), src_dir_(src_dir) {
    // 注册事件回调：关联到当前对象的处理函数
    channel_->SetReadCallback(std::bind(&TcpConnection::HandleRead, this));
    channel_->SetWriteCallback(std::bind(&TcpConnection::HandleWrite, this));
    channel_->SetCloseCallback(std::bind(&TcpConnection::HandleClose, this));
    channel_->SetErrorCallback(std::bind(&TcpConnection::HandleError, this));
}

/**
 * @brief TcpConnection析构函数：关闭连接fd（释放资源）
 */
TcpConnection::~TcpConnection() {
    close(fd_);
}

/**
 * @brief 连接建立初始化
 * 
 * 核心逻辑：
 * 1. 断言：必须在从Reactor线程执行
 * 2. 开启读事件（EPOLLIN+EPOLLET），注册到Epoller
 */
void TcpConnection::ConnectEstablished() {
    loop_->AssertInLoopThread();
    channel_->EnableReading(); // 开启读事件，等待客户端数据
}

/**
 * @brief 连接销毁（Server调用）
 * 
 * 核心逻辑：
 * 1. 断言：必须在从Reactor线程执行
 * 2. 禁用所有事件，从Epoller移除Channel
 */
void TcpConnection::ConnectDestroyed() {
    loop_->AssertInLoopThread();
    channel_->DisableAll(); // 禁用所有事件
    channel_->Remove();     // 从Epoller移除
}

/**
 * @brief 处理读事件（核心逻辑）
 * 
 * 流程：
 * 1. 循环读取数据（ET模式）→ 拼接为完整缓冲区
 * 2. 循环解析HTTP请求（处理粘包）
 * 3. 读取静态文件→构建HTTP响应→写入发送缓冲区
 * 4. 开启写事件（发送响应）
 */
void TcpConnection::HandleRead() {
    loop_->AssertInLoopThread();
    char buff[8192]; // 8KB缓冲区（平衡内存和系统调用）
    std::string all_data;
    int read_len = 0;

    // ET模式下必须循环读：直到EAGAIN/EWOULDBLOCK
    while (true) {
        memset(buff, 0, sizeof(buff));
        // 非阻塞读：recv返回-1且errno=EAGAIN时表示无数据
        read_len = recv(fd_, buff, sizeof(buff)-1, 0);
        if (read_len > 0) {
            all_data.append(buff, read_len); // 拼接数据
        } else if (read_len == 0) {
            // 客户端关闭连接（FIN）→ 触发关闭逻辑
            HandleClose();
            return;
        } else {
            // 非阻塞读的正常返回（无数据）
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 读错误→触发错误处理
            HandleError();
            return;
        }
    }

    // 无数据→关闭连接（避免空连接占用资源）
    if (all_data.empty()) {
        HandleClose();
        return;
    }

    // 循环解析：处理TCP粘包（一个缓冲区可能包含多个请求）
    bool has_more = true;
    while (has_more) {
        bool parse_ok = request_.Parse(all_data);
        if (parse_ok) {
            // 解析成功→获取请求信息
            std::string path = request_.path();
            bool keep_alive = request_.IsKeepAlive();
            // 提取剩余数据（粘包的下一个请求）
            all_data = request_.GetRemainingData();
            request_.ClearRemainingData();

            // ========== 读取静态文件 ==========
            std::string full_path = src_dir_ + path;
            std::string file_content;
            int code = 200; // 默认状态码200（OK）

            std::ifstream file(full_path);
            if (file.is_open()) {
                // 读取文件内容到缓冲区
                std::stringstream buffer;
                buffer << file.rdbuf();
                file_content = buffer.str();
                file.close();
            } else {
                // 文件不存在→404 Not Found
                code = 404;
            }

            // ========== 构建HTTP响应 ==========
            response_.Init(src_dir_, path, keep_alive, code);
            response_.MakeResponse(send_buffer_, file_content);
            request_.Init(); // 重置请求解析器（复用）

            // ========== 触发写事件 ==========
            if (!send_buffer_.empty()) {
                channel_->EnableWriting(); // 开启写事件，发送响应
            }

            // 检查是否有剩余数据（粘包）→ 继续解析
            has_more = !all_data.empty();
        } else {
            // 解析失败→停止循环（等待更多数据）
            has_more = false;
        }
    }
}

/**
 * @brief 处理写事件（发送HTTP响应）
 * 
 * 流程：
 * 1. 循环发送数据（ET模式）→ 直到发送缓冲区满/发送完成
 * 2. 发送完成→清空缓冲区，禁用写事件
 * 3. 非长连接→触发关闭逻辑
 */
void TcpConnection::HandleWrite() {
    loop_->AssertInLoopThread();
    if (send_buffer_.empty()) {
        // 无数据→禁用写事件
        channel_->DisableWriting();
        return;
    }

    int bytes_sent = 0;
    int bytes_to_send = send_buffer_.size();
    const char* ptr = send_buffer_.c_str();

    // 循环发送：直到所有数据发送完成/发送缓冲区满
    while (bytes_sent < bytes_to_send) {
        int len = send(fd_, ptr + bytes_sent, bytes_to_send - bytes_sent, 0);
        if (len > 0) {
            bytes_sent += len; // 累计发送字节数
        } else if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满→保存剩余数据，等待下次写事件
                send_buffer_ = std::string(ptr + bytes_sent, bytes_to_send - bytes_sent);
                return;
            }
            // 发送错误→触发错误处理
            HandleError();
            return;
        } else {
            // len=0→发送完成（正常情况）
            break;
        }
    }

    // 全部发送完成→清空缓冲区，禁用写事件
    send_buffer_.clear();
    channel_->DisableWriting();

    // 非长连接→关闭连接（短连接）
    if (!response_.IsKeepAlive()) {
        HandleClose();
    }
}

/**
 * @brief 处理关闭事件（通知Server）
 */
void TcpConnection::HandleClose() {
    loop_->AssertInLoopThread();
    if (close_callback_) {
        close_callback_(fd_); // 通知Server移除连接
    }
}

/**
 * @brief 处理错误事件（触发关闭）
 */
void TcpConnection::HandleError() {
    loop_->AssertInLoopThread();
    HandleClose(); // 错误事件直接关闭连接
}

} // namespace reactor