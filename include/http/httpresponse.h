#pragma once

/**
 * @file httpresponse.h
 * @brief HTTP响应类：构建符合HTTP/1.1规范的响应报文
 *
 * 核心功能：
 * 1. 初始化响应配置（资源目录、请求路径、长连接、状态码）
 * 2. 构建完整响应报文（状态行+响应头+响应体）
 * 3. 支持静态文件读取、MIME类型识别、长连接管理
 * 4. 支持零拷贝响应（仅响应头在用户态，响应体通过sendfile发送）
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
     * @brief 构建HTTP响应头（用于零拷贝发送）
     * @param response 输出参数：拼接后的响应头报文
     * @param file_size 响应体大小（用于设置Content-Length）
     */
    void MakeResponseHeader(std::string& response, size_t file_size = 0);

    /**
     * @brief 构建错误响应（用于404/500等错误）
     * @param response 输出参数：拼接后的响应报文
     * @param error_code HTTP错误状态码
     * @param error_msg 错误信息内容
     */
    void MakeErrorResponse(std::string& response, int error_code, const std::string& error_msg);

    /**
     * @brief 获取文件路径（用于零拷贝sendfile）
     * @return const std::string& 文件完整路径
     */
    const std::string& GetFilePath() const { return file_path_; }

    /**
     * @brief 获取文件大小（用于零拷贝sendfile）
     * @return size_t 文件大小
     */
    size_t GetFileSize() const { return static_cast<size_t>(mmFileStat_.st_size); }

    /**
     * @brief 判断是否可以使用零拷贝
     * @return bool true=可以使用零拷贝
     */
    bool IsZeroCopy() const { return is_static_file_ && code_ == 200 && S_ISREG(mmFileStat_.st_mode); }

    /**
     * @brief 获取文件状态（stat结构体）
     * @return struct stat 文件状态信息
     */
    struct stat FileStat() const { return mmFileStat_; }

    /**
     * @brief 设置自定义Content-Type（覆盖自动检测）
     * @param content_type MIME类型字符串
     */
    void SetContentType(const std::string& content_type) { content_type_override_ = content_type; }

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
     * @param content_length 响应体大小（0表示未知）
     */
    void AddHeader(std::string& response, size_t content_length = 0);

    /**
     * @brief 根据文件后缀获取MIME类型
     * @return std::string MIME类型字符串（如text/html）
     */
    std::string GetFileType();

    // ========== 成员变量 ==========
    int code_;                  // HTTP状态码（200/404/500等）
    bool isKeepAlive_;          // 是否开启长连接
    bool is_static_file_;      // 是否为静态文件响应
    std::string path_;          // 请求的文件路径（相对srcDir_）
    std::string srcDir_;        // 静态资源根目录
    std::string file_path_;     // 完整文件路径（用于零拷贝）
    std::string content_type_override_; // 自定义Content-Type（覆盖自动检测）
    struct stat mmFileStat_;    // 文件状态（大小、类型、权限等）

    // 静态常量：MIME类型映射（文件后缀→Content-Type）
    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    // 静态常量：状态码映射（状态码→描述信息）
    static const std::unordered_map<int, std::string> CODE_STATUS;
};

} // namespace reactor