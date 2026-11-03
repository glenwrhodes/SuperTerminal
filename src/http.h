#pragma once
#include <memory>
#include <string>
#include <vector>

namespace st {

struct HttpResult {
    bool sent = false;   // false: the request never got a response (network/TLS error)
    std::string error;   // why it wasn't sent
    int status = 0;
    std::string body;
};

// Minimal HTTPS client for one host, reusing its connection between calls.
// Windows: WinHTTP. macOS/Linux: libcurl. Safe to call from several threads.
class HttpClient {
public:
    explicit HttpClient(const std::string& host);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResult request(const std::string& method, const std::string& path, const std::string& body,
                       const std::vector<std::string>& headers);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace st
