/**
 * @file httprequest.cpp
 * @brief HTTP 请求解析实现：支持请求行、头部解析与 TCP 粘包处理
 *
 * 核心设计：
 * 1. 有限状态机（FSM）解析：REQUEST_LINE → HEADERS → BODY → FINISH
 * 2. TCP 粘包处理：保留剩余未解析数据（remaining_data_），下次拼接解析
 * 3. 请求头大小写不敏感：所有 key 转小写存储
 * 4. 路径规范化：默认 / 映射到 /index.html
 */

#include "http/httprequest.h"
#include <iostream>

namespace reactor {

/**
 * @brief 字符串转小写（处理 HTTP 请求头大小写不敏感）
 * @param str 输入字符串
 * @return std::string 转小写后的字符串
 *
 * HTTP 协议要求：请求头 key 大小写不敏感（如 Connection 和 connection 等价）
 */
std::string HttpRequest::ToLower(const std::string& str) {
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

/**
 * @brief 重置解析状态（复用 HttpRequest 对象）
 *
 * 【关键】不清空 remaining_data_：保留粘包的下一个请求数据
 * 重置内容：
 * - 方法、路径、版本、请求体清空
 * - 解析状态重置为 REQUEST_LINE
 * - 请求头 map 清空
 */
void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    header_.clear();
    // 【注意】不清空 remaining_data_，保留粘包的下一个请求数据
}

/**
 * @brief 解析 HTTP 请求（核心逻辑：有限状态机 + 粘包处理）
 * @param buff 新接收的 TCP 数据
 * @return bool 解析是否完成（true=完成，false=需继续读取）
 *
 * 解析流程：
 * 1. 拼接上次剩余的粘包数据（remaining_data_）和新数据
 * 2. 按 \r\n 分割行，逐行解析
 * 3. 有限状态机切换：请求行 → 请求头 → 完成
 * 4. 数据不完整时保存剩余数据，下次继续解析
 */
bool HttpRequest::Parse(const std::string& buff) {
    if (buff.empty() && remaining_data_.empty()) {
        return false;
        
    }

    // ========== 粘包处理：拼接上次剩余数据和新数据 ==========
    std::string buffer = remaining_data_ + buff;
    remaining_data_.clear();

    std::string line;
    size_t pos = 0;

    // ========== 有限状态机解析 ==========
    while (state_ != FINISH) {
        // 按 \r\n 分割行（HTTP 协议要求行尾必须是 \r\n）
        pos = buffer.find("\r\n");
        if (pos == std::string::npos && state_ != BODY) {
            // 数据不完整：保存剩余数据，下次解析
            remaining_data_ = buffer;
            return false;
        }

        if(state_ != BODY){
            // 提取单行，移除已解析部分
            line = buffer.substr(0, pos);
            buffer = buffer.substr(pos + 2);
        }
        // 状态机切换
        switch (state_) {
            case REQUEST_LINE:
                // 解析请求行：GET /index.html HTTP/1.1
                if (!ParseRequestLine(line)) {
                    return false;
                }
                ParsePath(); // 路径规范化
                state_ = HEADERS;
                break;

            case HEADERS:
                if (line.empty()) {
                    // 空行：请求头结束
                    if (method_ == "POST") {
                        // POST 请求需要解析请求体
                        state_ = BODY;
                    } else {
                        // GET 请求直接完成
                        state_ = FINISH;
                        remaining_data_ = buffer; // 保存粘包的下一个请求
                    }
                } else {
                    ParseHeader(line); // 解析单个请求头
                }
                break;

            case BODY:
                if (header_.count("content-length")) {
                    size_t content_length = 0;
                    try {
                        content_length = static_cast<size_t>(std::stoul(header_.at("content-length")));
                    } catch (...) {
                        return false;
                    }
                    if (content_length > 1048576) {
                        return false;
                    }
                    if (buffer.size() >= content_length) {
                        body_ = buffer.substr(0, content_length);
                        remaining_data_ = buffer.substr(content_length);
                    } else {
                        // 数据不完整：保存剩余数据，下次解析
                        remaining_data_ = buffer;
                        return false;
                    }
                }
                state_ = FINISH;
                break;

            default:
                break;
        }
    }
    return true;
}

/**
 * @brief 解析 HTTP 请求行（正则匹配）
 * @param line 请求行字符串
 * @return bool 解析是否成功
 *
 * 请求行格式：[方法] [路径] HTTP/[版本]
 * 示例：GET /index.html HTTP/1.1
 */
bool HttpRequest::ParseRequestLine(const std::string& line) {
    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) return false;
    
    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) return false;
    
    size_t http_pos = line.find("HTTP/", second_space + 1);
    if (http_pos == std::string::npos || http_pos != second_space + 1) return false;
    
    method_ = line.substr(0, first_space);
    path_ = line.substr(first_space + 1, second_space - first_space - 1);
    version_ = line.substr(http_pos + 5);
    return true;
}

/**
 * @brief 路径规范化：默认 / 映射到 /index.html
 */
void HttpRequest::ParsePath() {
    if (path_ == "/") {
        path_ = "/index.html";
    }
    if (path_.find("..") != std::string::npos) {
        path_ = "/index.html";
    }
    if (!path_.empty() && path_[0] != '/') {
        path_ = "/index.html";
    }
}

/**
 * @brief 解析单个 HTTP 请求头（正则匹配）
 * @param line 请求头行字符串
 *
 * 请求头格式：[Key]: [Value]
 * 示例：Connection: keep-alive
 *
 * 【关键】Key 转小写：解决 HTTP 请求头大小写不敏感问题
 */
void HttpRequest::ParseHeader(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) return;
    
    size_t key_end = colon_pos;
    while (key_end > 0 && line[key_end - 1] == ' ') key_end--;
    
    size_t value_start = colon_pos + 1;
    while (value_start < line.size() && line[value_start] == ' ') value_start++;
    
    std::string key = ToLower(line.substr(0, key_end));
    std::string value = line.substr(value_start);
    header_[key] = value;
}

// ========== Getter 函数：获取解析结果 ==========
std::string HttpRequest::path() const {
    return path_;
}

std::string HttpRequest::method() const {
    return method_;
}

std::string HttpRequest::version() const {
    return version_;
}

/**
 * @brief 判断是否开启长连接（Keep-Alive）
 * @return bool true=长连接，false=短连接
 *
 * 判断逻辑：
 * 1. 优先读取 Connection 请求头
 * 2. HTTP/1.1 默认开启长连接（即使没有 Connection 头）
 */
bool HttpRequest::IsKeepAlive() const {
    // 优先读取 Connection 头部
    if (header_.count("connection")) {
        return header_.at("connection") == "keep-alive";
    }
    // HTTP/1.1 默认开启长连接
    if (version_ == "1.1") {
        return true;
    }
    return false;
}

/**
 * @brief 获取请求体
 * @return std::string 请求体内容
 */
std::string HttpRequest::body() const {
    return body_;
}

} // namespace reactor