#pragma once

#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace reactor {

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    void Init(const std::string& srcDir, std::string& path, bool isKeepAlive = false, int code = -1);
    void MakeResponse(std::string& response, const std::string& cachedContent = "");

    struct stat FileStat() { return mmFileStat_; }
    bool IsKeepAlive() const { return isKeepAlive_; }

private:
    void AddStateLine(std::string& response);
    void AddHeader(std::string& response);
    void AddContent(std::string& response, const std::string& cachedContent);
    std::string GetFileType();

    int code_;
    bool isKeepAlive_;
    std::string path_;
    std::string srcDir_;
    struct stat mmFileStat_;

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    static const std::unordered_map<int, std::string> CODE_STATUS;
};

} // namespace reactor
