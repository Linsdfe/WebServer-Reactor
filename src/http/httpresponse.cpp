/**
 * @file httpresponse.cpp
 * @brief HTTP 响应构造实现：生成状态行、头部和正文
 *
 * 核心功能：
 * 1. 生成标准 HTTP/1.1 响应报文
 * 2. 支持 MIME 类型映射（文件后缀 → Content-Type）
 * 3. 支持状态码映射（200/404/500 等）
 * 4. 性能优化：预分配内存、优先使用缓存内容
 * 5. 长连接支持：Connection 和 Keep-Alive 头
 */

#include "http/httpresponse.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>

namespace reactor {

// ========== 静态成员初始化 ==========
/**
 * @brief 文件后缀 → MIME 类型映射（静态常量）
 *
 * 注意：
 * - 文本类型添加 charset=utf-8，避免中文乱码
 * - 常见文件类型全覆盖（html/css/js/png/jpg 等）
 */
const std::unordered_map<std::string, std::string> HttpResponse::SUFFIX_TYPE = {
    { ".html",  "text/html; charset=utf-8" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain; charset=utf-8" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".css",   "text/css" },
    { ".js",    "application/javascript" },
};

/**
 * @brief HTTP 状态码 → 描述映射（静态常量）
 *
 * 覆盖常见状态码：200（成功）、400（请求错误）、403（禁止）、404（未找到）、500（服务器错误）
 */
const std::unordered_map<int, std::string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
};

/**
 * @brief HttpResponse 构造函数：初始化默认值
 */
HttpResponse::HttpResponse() {
    code_ = -1;
    isKeepAlive_ = false;
    path_ = srcDir_ = "";
    memset(&mmFileStat_, 0, sizeof(mmFileStat_));
}

/**
 * @brief HttpResponse 析构函数（空实现，无动态资源需手动释放）
 */
HttpResponse::~HttpResponse() {}

/**
 * @brief 初始化响应配置
 * @param srcDir 静态资源根目录
 * @param path 请求的文件路径（相对 srcDir）
 * @param isKeepAlive 是否开启长连接
 * @param code HTTP 状态码（默认 -1，后续自动设为 200）
 */
void HttpResponse::Init(const std::string& srcDir, std::string& path, bool isKeepAlive, int code) {
    srcDir_ = srcDir;
    path_ = path;
    isKeepAlive_ = isKeepAlive;
    code_ = code;
    memset(&mmFileStat_, 0, sizeof(mmFileStat_));
}

/**
 * @brief 构建完整的 HTTP 响应报文（核心逻辑）
 * @param response 输出参数：拼接后的响应报文
 * @param cachedContent 可选：缓存的文件内容（优先使用，避免重复读盘）
 *
 * 【性能优化】
 * 1. 预分配内存：response.reserve(1024 + cachedContent.size())，减少字符串扩容开销
 * 2. 优先缓存内容：避免重复从磁盘读取文件
 *
 * HTTP 报文顺序（协议要求）：
 * 状态行 → 响应头 → 空行 → 响应体
 */
void HttpResponse::MakeResponse(std::string& response, const std::string& cachedContent) {
    // 默认状态码 200（OK）
    if (code_ == -1) {
        code_ = 200;
    }

    // 【性能优化】预分配内存，减少字符串扩容开销
    response.reserve(1024 + cachedContent.size());

    // 按 HTTP 协议顺序拼接报文
    AddStateLine(response);   // 1. 状态行
    AddHeader(response);      // 2. 响应头
    AddContent(response, cachedContent); // 3. 空行 + 响应体
}

/**
 * @brief 添加 HTTP 状态行
 * @param response 响应报文拼接缓冲区
 *
 * 状态行格式：HTTP/1.1 [状态码] [描述]
 * 示例：HTTP/1.1 200 OK
 *
 * 未知状态码处理：默认设为 400（Bad Request）
 */
void HttpResponse::AddStateLine(std::string& response) {
    std::string status;
    if (CODE_STATUS.count(code_)) {
        status = CODE_STATUS.find(code_)->second;
    } else {
        // 未知状态码：默认 400（Bad Request）
        code_ = 400;
        status = CODE_STATUS.find(400)->second;
    }
    response += "HTTP/1.1 " + std::to_string(code_) + " " + status + "\r\n";
}

/**
 * @brief 添加 HTTP 响应头
 * @param response 响应报文拼接缓冲区
 *
 * 添加的响应头：
 * 1. Connection：keep-alive / close
 * 2. Keep-Alive：timeout=60, max=1000（长连接时）
 * 3. Content-Type：根据文件后缀映射 MIME 类型
 */
void HttpResponse::AddHeader(std::string& response) {
    response += "Connection: ";
    if (isKeepAlive_) {
        // 长连接：设置超时时间和最大请求数
        response += "keep-alive\r\n";
        response += "Keep-Alive: timeout=60, max=1000\r\n";
    } else {
        // 短连接：关闭连接
        response += "close\r\n";
    }
    // Content-Type：根据文件后缀映射
    response += "Content-Type: " + GetFileType() + "\r\n";
}

/**
 * @brief 添加 HTTP 响应体（含空行分隔）
 * @param response 响应报文拼接缓冲区
 * @param cachedContent 缓存的文件内容（优先使用）
 *
 * 【关键】HTTP 协议要求：响应头和响应体之间必须有一个空行（\r\n\r\n）
 *
 * 逻辑：
 * 1. 优先使用缓存内容（cachedContent）
 * 2. 兜底：从磁盘读取文件
 * 3. 文件不存在：返回 404，Content-Length=0
 */
void HttpResponse::AddContent(std::string& response, const std::string& cachedContent) {
    // ========== 优先使用缓存内容 ==========
    if (!cachedContent.empty()) {
        response += "Content-Length: " + std::to_string(cachedContent.size()) + "\r\n";
        response += "\r\n"; // 【关键】头部和 Body 之间必须有空行
        response += cachedContent;
        return;
    }

    // ========== 兜底：从磁盘读取文件 ==========
    std::string fullPath = srcDir_ + path_;
    std::ifstream file(fullPath);

    if (!file.is_open()) {
        // 文件不存在：返回 404，Content-Length=0
        code_ = 404;
        response += "Content-Length: 0\r\n\r\n";
        return;
    }

    // 读取文件内容到字符串
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fileContent = buffer.str();
    file.close();

    // 拼接 Content-Length、空行和响应体
    response += "Content-Length: " + std::to_string(fileContent.size()) + "\r\n";
    response += "\r\n";
    response += fileContent;
}

/**
 * @brief 获取文件对应的 MIME 类型（Content-Type）
 * @return std::string MIME 类型字符串
 *
 * 逻辑：
 * 1. 查找文件最后一个 . 的位置，提取后缀
 * 2. 在 SUFFIX_TYPE 中查找对应的 MIME 类型
 * 3. 未找到：默认返回 text/plain; charset=utf-8
 */
std::string HttpResponse::GetFileType() {
    // 查找文件最后一个 . 的位置
    std::string::size_type idx = path_.find_last_of('.');
    if (idx == std::string::npos) {
        // 无后缀：默认 text/plain
        return "text/plain; charset=utf-8";
    }

    // 提取后缀并查找映射
    std::string suffix = path_.substr(idx);
    if (SUFFIX_TYPE.count(suffix)) {
        return SUFFIX_TYPE.find(suffix)->second;
    }

    // 未知后缀：默认 text/plain
    return "text/plain; charset=utf-8";
}

} // namespace reactor