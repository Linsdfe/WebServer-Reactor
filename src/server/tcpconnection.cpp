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
 * @param mysql_host MySQL主机地址
 * @param mysql_user MySQL用户名
 * @param mysql_password MySQL密码
 * @param mysql_database MySQL数据库名
 * 
 * 初始化流程：
 * 1. 保存基本参数（loop/fd/src_dir）
 * 2. 创建Channel管理连接fd
 * 3. 注册所有事件回调（读/写/关闭/错误）
 * 4. 初始化Auth模块
 */
TcpConnection::TcpConnection(EventLoop* loop, int fd, const std::string& src_dir, 
                             const std::string& mysql_host, const std::string& mysql_user, 
                             const std::string& mysql_password, const std::string& mysql_database,
                             const std::shared_ptr<CacheManager>& cache_manager)
    : loop_(loop), channel_(new Channel(loop, fd)), fd_(fd), src_dir_(src_dir),
      auth_(mysql_host, mysql_user, mysql_password, mysql_database),
      cache_manager_(cache_manager) {
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
    char buff[65536];
    std::string all_data;
    all_data.reserve(16384);
    ssize_t read_len = 0;

    // ET模式下必须循环读：直到EAGAIN/EWOULDBLOCK
    while (true) {
        // 非阻塞读：recv返回-1且errno=EAGAIN时表示无数据
        read_len = recv(fd_, buff, sizeof(buff), 0);
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
            std::string method = request_.method();
            bool keep_alive = request_.IsKeepAlive();
            // 提取剩余数据（粘包的下一个请求）
            all_data = request_.GetRemainingData();
            request_.ClearRemainingData();

            // ========== 处理登录和注册请求 ==========
            //std::cout << "[INFO] 收到请求：方法=" << method << ", 路径=" << path << std::endl;
            if (method == "POST" && path == "/login") {
                //std::cout << "[INFO] 处理登录请求" << std::endl;
                // 解析请求体，获取用户名和密码
                std::string body = request_.body();
                //std::cout << "[INFO] 请求体：" << body << std::endl;
                std::string username, password;
                
                // 解析表单数据：username=xxx&password=xxx
                size_t pos = body.find("&");
                if (pos != std::string::npos) {
                    std::string user_part = body.substr(0, pos);
                    std::string pass_part = body.substr(pos + 1);
                    
                    size_t user_eq = user_part.find("=");
                    size_t pass_eq = pass_part.find("=");
                    
                    if (user_eq != std::string::npos && pass_eq != std::string::npos) {
                        username = user_part.substr(user_eq + 1);
                        password = pass_part.substr(pass_eq + 1);
                    }
                }
                
                //std::cout << "[INFO] 登录请求：用户名=" << username << ", 密码=" << (password.empty() ? "空" : "***") << std::endl;
                // 验证用户
                bool success = auth_.ValidateUser(username, password);
                
                // 构建JSON响应
                std::string response_json;
                if (success) {
                    std::string session_id = auth_.GenerateSessionId(username);
                    response_json = "{\"success\": true, \"message\": \"登录成功\", \"session_id\": \"" + session_id + "\"}";
                    //std::cout << "[INFO] 用户登录成功：" << username << std::endl;
                } else {
                    response_json = "{\"success\": false, \"message\": \"用户名或密码错误\"}";
                    //std::cout << "[INFO] 用户登录失败：" << username << std::endl;
                }
                
                // 构建HTTP响应
                response_.Init(src_dir_, path, keep_alive, 200);
                response_.MakeResponse(send_buffer_, response_json);
                request_.Init(); // 重置请求解析器（复用）
                //std::cout << "[INFO] 登录响应已发送" << std::endl;
            } else if (method == "POST" && path == "/register") {
                //std::cout << "[INFO] 处理注册请求" << std::endl;
                // 解析请求体，获取用户名和密码
                std::string body = request_.body();
                //std::cout << "[INFO] 请求体：" << body << std::endl;
                std::string username, password;
                
                // 解析表单数据：username=xxx&password=xxx
                size_t pos = body.find("&");
                if (pos != std::string::npos) {
                    std::string user_part = body.substr(0, pos);
                    std::string pass_part = body.substr(pos + 1);
                    
                    size_t user_eq = user_part.find("=");
                    size_t pass_eq = pass_part.find("=");
                    
                    if (user_eq != std::string::npos && pass_eq != std::string::npos) {
                        username = user_part.substr(user_eq + 1);
                        password = pass_part.substr(pass_eq + 1);
                    }
                }
                
                //std::cout << "[INFO] 注册请求：用户名=" << username << ", 密码=" << (password.empty() ? "空" : "***") << std::endl;
                // 添加用户
                bool success = auth_.AddUser(username, password);
                
                // 构建JSON响应
                std::string response_json;
                if (success) {
                    response_json = "{\"success\": true, \"message\": \"注册成功\"}";
                    //std::cout << "[INFO] 用户注册成功：" << username << std::endl;
                } else {
                    response_json = "{\"success\": false, \"message\": \"注册失败，用户名可能已存在\"}";
                    //std::cout << "[INFO] 用户注册失败：" << username << std::endl;
                }
                
                // 构建HTTP响应
                response_.Init(src_dir_, path, keep_alive, 200);
                response_.MakeResponse(send_buffer_, response_json);
                request_.Init(); // 重置请求解析器（复用）
                //std::cout << "[INFO] 注册响应已发送" << std::endl;
            } else {
                // ========== 读取静态文件（使用缓存） ==========
                std::string full_path = src_dir_ + path;
                std::string file_content;
                int code = 200; // 默认状态码200（OK）

                // 1. 尝试从缓存获取
                if (cache_manager_ && cache_manager_->GetCache(full_path, file_content)) {
                    // 缓存命中
                } else {
                    // 2. 缓存未命中，从磁盘读取
                    struct stat file_stat;
                    if (stat(full_path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
                        code = 404; // 文件不存在或不是普通文件
                    } else {
                        // 二进制模式打开文件（避免文本模式转换）
                        std::ifstream file(full_path, std::ios::binary);
                        if (file.is_open()) {
                            // 预分配内存，避免动态扩容
                            file_content.resize(file_stat.st_size);
                            // 直接读取到 string 缓冲区（零中间拷贝）
                            file.read(&file_content[0], file_stat.st_size);
                            
                            // 检查读取是否成功（如文件中途被截断）
                            if (!file) {
                                code = 500; // 内部服务器错误
                                file_content.clear();
                            } else {
                                // 3. 将文件内容添加到缓存
                                if (cache_manager_) {
                                    cache_manager_->SetCache(full_path, file_content);
                                }
                            }
                            file.close();
                        } else {
                            code = 404;
                        }
                    }
                }


                // ========== 构建HTTP响应 ==========
                response_.Init(src_dir_, path, keep_alive, code);
                response_.MakeResponse(send_buffer_, std::move(file_content));
                request_.Init(); // 重置请求解析器（复用）
            }

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