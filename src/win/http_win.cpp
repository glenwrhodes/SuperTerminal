#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include "util.h"

namespace st {

struct HttpClient::Impl {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
};

HttpClient::HttpClient(const std::string& host) : impl_(std::make_unique<Impl>()) {
    impl_->session = WinHttpOpen(L"SuperTerminal/" ST_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (impl_->session) {
        WinHttpSetTimeouts(impl_->session, 10000, 10000, 30000, 120000);
        DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(impl_->session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof http2);
        impl_->connection = WinHttpConnect(impl_->session, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
}

HttpClient::~HttpClient() {
    if (impl_->connection) WinHttpCloseHandle(impl_->connection);
    if (impl_->session) WinHttpCloseHandle(impl_->session);
}

HttpResult HttpClient::request(const std::string& method, const std::string& path, const std::string& body,
                               const std::vector<std::string>& headers) {
    HttpResult r;
    if (!impl_->connection) {
        r.error = win32_error(GetLastError());
        return r;
    }
    HINTERNET req = WinHttpOpenRequest(impl_->connection, widen(method).c_str(), widen(path).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        r.error = win32_error(GetLastError());
        return r;
    }
    std::wstring header_block;
    for (const auto& h : headers) header_block += widen(h) + L"\r\n";

    BOOL ok = WinHttpSendRequest(req, header_block.c_str(), static_cast<DWORD>(-1L),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        r.error = win32_error(GetLastError());
        WinHttpCloseHandle(req);
        return r;
    }
    r.sent = true;
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        r.body.append(chunk.data(), read);
    }
    WinHttpCloseHandle(req);
    return r;
}

}  // namespace st
