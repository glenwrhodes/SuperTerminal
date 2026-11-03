#include "http.h"

#include <curl/curl.h>

#include <mutex>

namespace st {

namespace {
size_t collect(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}
std::once_flag g_curl_init;
}  // namespace

struct HttpClient::Impl {
    std::string base;
    std::mutex mu;       // one easy handle, reused so the TLS connection stays open
    CURL* curl = nullptr;
};

HttpClient::HttpClient(const std::string& host) : impl_(std::make_unique<Impl>()) {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->base = "https://" + host;
    impl_->curl = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (impl_->curl) curl_easy_cleanup(impl_->curl);
}

HttpResult HttpClient::request(const std::string& method, const std::string& path, const std::string& body,
                               const std::vector<std::string>& headers) {
    HttpResult r;
    std::lock_guard lock(impl_->mu);
    CURL* c = impl_->curl;
    if (!c) {
        r.error = "libcurl failed to initialise";
        return r;
    }
    curl_easy_reset(c);
    std::string url = impl_->base + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "SuperTerminal/" ST_VERSION);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 150L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(list);
    if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
        r.body.clear();
        return r;
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    r.sent = true;
    r.status = static_cast<int>(status);
    return r;
}

}  // namespace st
