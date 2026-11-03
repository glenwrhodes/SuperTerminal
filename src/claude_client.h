#pragma once
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace st {

class HttpClient;

// Calls the Claude Messages API directly over HTTPS. The key comes from the
// ANTHROPIC_API_KEY environment variable; nothing else is needed.
class ClaudeClient {
public:
    struct Reply {
        bool ok = false;
        bool refused = false;      // the model declined (stop_reason "refusal")
        std::string error;         // set when !ok; worded for the user
        nlohmann::json result;     // the structured JSON object returned
        std::string model;         // the model that answered
        long long elapsed_ms = 0;
        int input_tokens = 0, output_tokens = 0;
    };

    ClaudeClient();
    ~ClaudeClient();
    ClaudeClient(const ClaudeClient&) = delete;
    ClaudeClient& operator=(const ClaudeClient&) = delete;

    bool has_key() const { return !key_.empty(); }

    // One request with a JSON-schema-constrained reply.
    Reply ask(const std::string& model, const std::string& effort, const std::string& system,
              const std::string& user_text, const nlohmann::json& schema, int max_tokens = 8000);

    // Opens the TLS connection early so the first real request is faster.
    void warm_up(const std::string& model);

private:
    std::vector<std::string> headers() const;
    std::unique_ptr<HttpClient> http_;
    std::string key_;
};

}  // namespace st
