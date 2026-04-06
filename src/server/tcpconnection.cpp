#include "server/tcpconnection.h"
#include <unistd.h>
#include <errno.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/socket.h> // 【修复】添加recv/send定义头文件
#include <sys/types.h>

namespace reactor {

TcpConnection::TcpConnection(EventLoop* loop, int fd, const std::string& src_dir)
    : loop_(loop), channel_(new Channel(loop, fd)), fd_(fd), src_dir_(src_dir) {
    // 注册事件回调
    channel_->SetReadCallback(std::bind(&TcpConnection::HandleRead, this));
    channel_->SetWriteCallback(std::bind(&TcpConnection::HandleWrite, this));
    channel_->SetCloseCallback(std::bind(&TcpConnection::HandleClose, this));
    channel_->SetErrorCallback(std::bind(&TcpConnection::HandleError, this));
}

TcpConnection::~TcpConnection() {
    close(fd_);
}

void TcpConnection::ConnectEstablished() {
    loop_->AssertInLoopThread();
    channel_->EnableReading();
}

void TcpConnection::ConnectDestroyed() {
    loop_->AssertInLoopThread();
    channel_->DisableAll();
    channel_->Remove();
}

void TcpConnection::HandleRead() {
    loop_->AssertInLoopThread();
    char buff[8192];
    std::string all_data;
    int read_len = 0;

    // ET模式下必须循环读，直到没有数据
    while (true) {
        memset(buff, 0, sizeof(buff));
        read_len = recv(fd_, buff, sizeof(buff)-1, 0);
        if (read_len > 0) {
            all_data.append(buff, read_len);
        } else if (read_len == 0) {
            // 客户端关闭连接
            HandleClose();
            return;
        } else {
            // 非阻塞读，没有数据了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 读错误
            HandleError();
            return;
        }
    }

    if (all_data.empty()) {
        HandleClose();
        return;
    }

    // 循环解析，处理粘包
    bool has_more = true;
    while (has_more) {
        bool parse_ok = request_.Parse(all_data);
        if (parse_ok) {
            // 解析成功，获取请求信息
            std::string path = request_.path();
            bool keep_alive = request_.IsKeepAlive();
            all_data = request_.GetRemainingData();
            request_.ClearRemainingData();

            // 读取静态文件
            std::string full_path = src_dir_ + path;
            std::string file_content;
            int code = 200;

            std::ifstream file(full_path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                file_content = buffer.str();
                file.close();
            } else {
                code = 404;
            }

            // 生成HTTP响应
            response_.Init(src_dir_, path, keep_alive, code);
            response_.MakeResponse(send_buffer_, file_content);
            request_.Init();

            // 注册写事件，发送响应
            if (!send_buffer_.empty()) {
                channel_->EnableWriting();
            }

            // 检查是否还有剩余数据（下一个请求）
            has_more = !all_data.empty();
        } else {
            // 解析失败，停止循环
            has_more = false;
        }
    }
}

void TcpConnection::HandleWrite() {
    loop_->AssertInLoopThread();
    if (send_buffer_.empty()) {
        channel_->DisableWriting();
        return;
    }

    int bytes_sent = 0;
    int bytes_to_send = send_buffer_.size();
    const char* ptr = send_buffer_.c_str();

    // 循环发送数据
    while (bytes_sent < bytes_to_send) {
        int len = send(fd_, ptr + bytes_sent, bytes_to_send - bytes_sent, 0);
        if (len > 0) {
            bytes_sent += len;
        } else if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满了，保存剩余数据，等待下次写事件
                send_buffer_ = std::string(ptr + bytes_sent, bytes_to_send - bytes_sent);
                return;
            }
            // 发送错误
            HandleError();
            return;
        } else {
            // 发送完成
            break;
        }
    }

    // 全部发送完成
    send_buffer_.clear();
    channel_->DisableWriting();

    // 非长连接，关闭连接
    if (!response_.IsKeepAlive()) {
        HandleClose();
    }
}

void TcpConnection::HandleClose() {
    loop_->AssertInLoopThread();
    if (close_callback_) {
        close_callback_(fd_);
    }
}

void TcpConnection::HandleError() {
    loop_->AssertInLoopThread();
    HandleClose();
}

} // namespace reactor
