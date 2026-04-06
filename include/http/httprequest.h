#pragma once

#include <string>
#include <unordered_map>
#include <regex>
#include <algorithm>

namespace reactor {

class HttpRequest {
public:
    enum PARSE_STATE {
        REQUEST_LINE,
        HEADERS,
        BODY,
        FINISH
    };

    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    void Init();
    bool Parse(const std::string& buff);

    // 粘包处理接口
    std::string GetRemainingData() const { return remaining_data_; }
    void ClearRemainingData() { remaining_data_.clear(); }

    // 对外获取解析结果
    std::string path() const;
    std::string method() const;
    std::string version() const;
    bool IsKeepAlive() const;

private:
    bool ParseRequestLine(const std::string& line);
    void ParseHeader(const std::string& line);
    void ParsePath();
    std::string ToLower(const std::string& str);

    PARSE_STATE state_;
    std::string method_;
    std::string path_;
    std::string version_;
    std::string body_;
    std::string remaining_data_;
    std::unordered_map<std::string, std::string> header_;
};

} // namespace reactor
