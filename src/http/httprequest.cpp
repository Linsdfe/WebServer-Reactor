#include "http/httprequest.h"
#include <iostream>

namespace reactor {

std::string HttpRequest::ToLower(const std::string& str) {
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    header_.clear();
    // 不清空remaining_data_，保留粘包的下一个请求数据
}

bool HttpRequest::Parse(const std::string& buff) {
    if (buff.empty() && remaining_data_.empty()) {
        return false;
    }

    // 拼接上次剩余的粘包数据和新数据
    std::string buffer = remaining_data_ + buff;
    remaining_data_.clear();

    std::string line;
    size_t pos = 0;

    while (state_ != FINISH) {
        pos = buffer.find("\r\n");
        if (pos == std::string::npos) {
            // 数据不完整，保存剩余数据下次解析
            remaining_data_ = buffer;
            return false;
        }

        line = buffer.substr(0, pos);
        buffer = buffer.substr(pos + 2);

        switch (state_) {
            case REQUEST_LINE:
                if (!ParseRequestLine(line)) {
                    return false;
                }
                ParsePath();
                state_ = HEADERS;
                break;
            case HEADERS:
                if (line.empty()) {
                    // 头部结束，解析完成
                    state_ = FINISH;
                    remaining_data_ = buffer;
                } else {
                    ParseHeader(line);
                }
                break;
            case BODY:
                // 本项目暂不处理POST请求的Body
                state_ = FINISH;
                remaining_data_ = buffer;
                break;
            default:
                break;
        }
    }
    return true;
}

bool HttpRequest::ParseRequestLine(const std::string& line) {
    // 正则匹配请求行：GET /index.html HTTP/1.1
    std::regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    std::smatch subMatch;
    if (std::regex_match(line, subMatch, patten)) {
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        return true;
    }
    return false;
}

void HttpRequest::ParsePath() {
    // 默认访问index.html
    if (path_ == "/") {
        path_ = "/index.html";
    }
}

void HttpRequest::ParseHeader(const std::string& line) {
    // 正则匹配头部：Connection: keep-alive
    std::regex patten("^([^:]*): ?(.*)$");
    std::smatch subMatch;
    if (std::regex_match(line, subMatch, patten)) {
        // 头部key转小写，解决大小写敏感问题
        std::string key = ToLower(subMatch[1]);
        std::string value = ToLower(subMatch[2]);
        header_[key] = value;
    }
}

std::string HttpRequest::path() const {
    return path_;
}

std::string HttpRequest::method() const {
    return method_;
}

std::string HttpRequest::version() const {
    return version_;
}

bool HttpRequest::IsKeepAlive() const {
    // 优先读取Connection头部
    if (header_.count("connection")) {
        return header_.at("connection") == "keep-alive";
    }
    // HTTP/1.1默认开启长连接
    if (version_ == "1.1") {
        return true;
    }
    return false;
}

} // namespace reactor
