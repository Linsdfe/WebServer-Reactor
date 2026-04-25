#pragma once

/**
 * @file httprequest.h
 * @brief HTTP请求类：解析HTTP/1.1请求报文，处理粘包
 * 
 * 核心功能：
 * 1. 分阶段解析请求（请求行→请求头→请求体→完成）
 * 2. 处理TCP粘包（保存剩余数据，下次解析）
 * 3. 解析请求方法、路径、版本、头部（支持Keep-Alive）
 * 4. 路径规范化（/ → /index.html）
 */
#include <string>
#include <unordered_map>
#include <algorithm>

namespace reactor {

class HttpRequest {
public:
    /**
     * @brief 解析状态枚举（有限状态机）
     */
    enum PARSE_STATE {
        REQUEST_LINE,  // 解析请求行（GET /index.html HTTP/1.1）
        HEADERS,       // 解析请求头（Connection: keep-alive等）
        BODY,          // 解析请求体（POST 表单数据）
        FINISH         // 解析完成
    };

    /**
     * @brief 构造函数：初始化解析状态和成员变量
     */
    HttpRequest() { Init(); }
    
    /**
     * @brief 析构函数（默认实现）
     */
    ~HttpRequest() = default;

    /**
     * @brief 重置解析状态（复用HttpRequest对象）
     */
    void Init();
    
    /**
     * @brief 解析HTTP请求报文（处理粘包）
     * @param buff 待解析的缓冲区（TCP读取的数据）
     * @return bool 解析是否完成（true=完成，false=需继续读取）
     */
    bool Parse(const std::string& buff);

    // ========== 粘包处理接口 ==========
    /**
     * @brief 获取剩余未解析数据（粘包的下一个请求）
     * @return std::string 剩余数据
     */
    std::string GetRemainingData() const { return remaining_data_; }
    
    /**
     * @brief 清空剩余数据（解析完成后调用）
     */
    void ClearRemainingData() { remaining_data_.clear(); }

    // ========== 解析结果获取接口 ==========
    /**
     * @brief 获取请求路径
     * @return std::string 请求路径（如/index.html）
     */
    std::string path() const;
    
    /**
     * @brief 获取请求方法
     * @return std::string 请求方法（如GET）
     */
    std::string method() const;
    
    /**
     * @brief 获取HTTP版本
     * @return std::string HTTP版本（如1.1）
     */
    std::string version() const;
    
    /**
     * @brief 判断是否开启长连接
     * @return bool true=长连接，false=短连接
     */
    bool IsKeepAlive() const;

    /**
     * @brief 获取请求体
     * @return std::string 请求体内容
     */
    std::string body() const;

private:
    /**
     * @brief 解析请求行
     * @param line 单行数据（\r\n分隔）
     * @return bool 解析是否成功
     */
    bool ParseRequestLine(const std::string& line);
    
    /**
     * @brief 解析请求头
     * @param line 单行数据（\r\n分隔）
     */
    void ParseHeader(const std::string& line);
    
    /**
     * @brief 路径规范化（/ → /index.html）
     */
    void ParsePath();
    
    /**
     * @brief 字符串转小写（处理请求头大小写不敏感）
     * @param str 输入字符串
     * @return std::string 小写字符串
     */
    std::string ToLower(const std::string& str);

    // ========== 成员变量 ==========
    PARSE_STATE state_;          // 当前解析状态
    std::string method_;         // 请求方法（GET/POST等）
    std::string path_;           // 请求路径
    std::string version_;        // HTTP版本（1.0/1.1）
    std::string body_;           // 请求体（POST 表单数据）
    std::string remaining_data_; // 粘包剩余数据（未解析）
    std::unordered_map<std::string, std::string> header_; // 请求头（key:value）
};

} // namespace reactor