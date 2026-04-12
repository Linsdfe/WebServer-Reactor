#pragma once

/**
 * @file httpresponse.h
 * @brief HTTP响应类：构建符合HTTP/1.1规范的响应报文
 * 
 * 核心功能：
 * 1. 初始化响应配置（资源目录、请求路径、长连接、状态码）
 * 2. 构建完整响应报文（状态行+响应头+响应体）
 * 3. 支持静态文件读取、MIME类型识别、长连接管理
 */
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace reactor {

class HttpResponse {
public:
    /**
     * @brief 构造函数：初始化默认值
     */
    HttpResponse();
    /**
     * @brief 析构函数（空实现）
     */
    ~HttpResponse();

    /**
     * @brief 初始化响应配置
     * @param srcDir 静态资源根目录
     * @param path 请求的文件路径（相对srcDir）
     * @param isKeepAlive 是否开启长连接
     * @param code HTTP状态码（默认-1，自动设为200）
     */
    void Init(const std::string& srcDir, std::string& path, bool isKeepAlive = false, int code = -1);
    
    /**
     * @brief 构建完整的HTTP响应报文
     * @param response 输出参数：拼接后的响应报文
     * @param cachedContent 可选：缓存的文件内容（避免重复读取）
     */
    void MakeResponse(std::string& response, const std::string& cachedContent = "");

    /**
     * @brief 获取文件状态（stat结构体）
     * @return struct stat 文件状态信息
     */
    struct stat FileStat() { return mmFileStat_; }
    
    /**
     * @brief 判断是否开启长连接
     * @return bool true=长连接，false=短连接
     */
    bool IsKeepAlive() const { return isKeepAlive_; }

private:
    /**
     * @brief 拼接状态行（HTTP/1.1 200 OK）
     * @param response 响应报文拼接缓冲区
     */
    void AddStateLine(std::string& response);
    
    /**
     * @brief 拼接响应头（Connection/Content-Type等）
     * @param response 响应报文拼接缓冲区
     */
    void AddHeader(std::string& response);
    
    /**
     * @brief 拼接响应体（静态文件内容）
     * @param response 响应报文拼接缓冲区
     * @param cachedContent 缓存的文件内容（优先使用）
     */
    void AddContent(std::string& response, const std::string& cachedContent);
    
    /**
     * @brief 根据文件后缀获取MIME类型
     * @return std::string MIME类型字符串（如text/html）
     */
    std::string GetFileType();

    // ========== 成员变量 ==========
    int code_;                  // HTTP状态码（200/404/500等）
    bool isKeepAlive_;          // 是否开启长连接
    std::string path_;          // 请求的文件路径（相对srcDir_）
    std::string srcDir_;        // 静态资源根目录
    struct stat mmFileStat_;    // 文件状态（大小、类型、权限等）

    // 静态常量：MIME类型映射（文件后缀→Content-Type）
    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    // 静态常量：状态码映射（状态码→描述信息）
    static const std::unordered_map<int, std::string> CODE_STATUS;
};

} // namespace reactor