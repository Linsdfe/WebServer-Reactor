#include "http/httpresponse.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>

namespace reactor {

// 静态成员初始化：文件后缀 -> MIME类型
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

// 静态成员初始化：状态码 -> 描述
const std::unordered_map<int, std::string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
};

HttpResponse::HttpResponse() {
    code_ = -1;
    isKeepAlive_ = false;
    path_ = srcDir_ = "";
    memset(&mmFileStat_, 0, sizeof(mmFileStat_));
}

HttpResponse::~HttpResponse() {}

void HttpResponse::Init(const std::string& srcDir, std::string& path, bool isKeepAlive, int code) {
    srcDir_ = srcDir;
    path_ = path;
    isKeepAlive_ = isKeepAlive;
    code_ = code;
    memset(&mmFileStat_, 0, sizeof(mmFileStat_));
}

void HttpResponse::MakeResponse(std::string& response, const std::string& cachedContent) {
    // 默认状态码200
    if (code_ == -1) {
        code_ = 200;
    }

    // 预分配内存，减少字符串扩容开销
    response.reserve(1024 + cachedContent.size());

    // 按HTTP协议顺序拼接报文
    AddStateLine(response);
    AddHeader(response);
    AddContent(response, cachedContent);
}

void HttpResponse::AddStateLine(std::string& response) {
    std::string status;
    if (CODE_STATUS.count(code_)) {
        status = CODE_STATUS.find(code_)->second;
    } else {
        code_ = 400;
        status = CODE_STATUS.find(400)->second;
    }
    response += "HTTP/1.1 " + std::to_string(code_) + " " + status + "\r\n";
}

void HttpResponse::AddHeader(std::string& response) {
    response += "Connection: ";
    if (isKeepAlive_) {
        response += "keep-alive\r\n";
        response += "Keep-Alive: timeout=60, max=1000\r\n";
    } else {
        response += "close\r\n";
    }
    response += "Content-Type: " + GetFileType() + "\r\n";
}

void HttpResponse::AddContent(std::string& response, const std::string& cachedContent) {
    // 优先使用缓存内容
    if (!cachedContent.empty()) {
        response += "Content-Length: " + std::to_string(cachedContent.size()) + "\r\n";
        response += "\r\n"; // 头部和Body之间必须有空行
        response += cachedContent;
        return;
    }

    // 兜底：从磁盘读取文件
    std::string fullPath = srcDir_ + path_;
    std::ifstream file(fullPath);

    if (!file.is_open()) {
        code_ = 404;
        response += "Content-Length: 0\r\n\r\n";
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fileContent = buffer.str();
    file.close();

    response += "Content-Length: " + std::to_string(fileContent.size()) + "\r\n";
    response += "\r\n";
    response += fileContent;
}

std::string HttpResponse::GetFileType() {
    // 查找文件后缀
    std::string::size_type idx = path_.find_last_of('.');
    if (idx == std::string::npos) {
        return "text/plain; charset=utf-8";
    }

    std::string suffix = path_.substr(idx);
    if (SUFFIX_TYPE.count(suffix)) {
        return SUFFIX_TYPE.find(suffix)->second;
    }

    return "text/plain; charset=utf-8";
}

} // namespace reactor
