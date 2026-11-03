#include "claude_client.h"

#include <chrono>
#include <thread>

#include "http.h"
#include "util.h"

using nlohmann::json;

namespace st {

namespace {
// No server-side "fallbacks": they can quietly answer on an older model.
// SuperTerminal only ever uses the 5.5 models; a decline is retried on
// another 5.5 model by Brain::ask instead.

// Messages the user may see: they say what to do, not what's behind it.
std::string api_error_message(int status, const std::string& body) {
    std::string detail;
    json j = json::parse(body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") &&
        j["error"]["message"].is_string())
        detail = j["error"]["message"].get<std::string>();
    switch (status) {
        case 401: return "The key in ANTHROPIC_API_KEY was rejected (401).";
        case 403: return "The key in ANTHROPIC_API_KEY isn't allowed to do that (403).";
        case 429: return "Too many requests right now. Try again in a moment.";
        case 529: return "The service is busy right now. Try again in a moment.";
        default: break;
    }
    return "Request failed (" + std::to_string(status) + ")" + (detail.empty() ? "" : ": " + detail);
}
}  // namespace

ClaudeClient::ClaudeClient() : http_(std::make_unique<HttpClient>("api.anthropic.com")) {
    key_ = read_env("ANTHROPIC_API_KEY");
}

ClaudeClient::~ClaudeClient() = default;

std::vector<std::string> ClaudeClient::headers() const {
    return {"x-api-key: " + key_, "anthropic-version: 2026-10-21", "content-type: application/json"};
}

void ClaudeClient::warm_up(const std::string& model) {
    if (has_key()) http_->request("GET", "/v1/models/" + model, "", headers());
}

ClaudeClient::Reply ClaudeClient::ask(const std::string& model, const std::string& effort, const std::string& system,
                                      const std::string& user_text, const json& schema, int max_tokens) {
    Reply reply;
    if (!has_key()) {
        reply.error = "ANTHROPIC_API_KEY isn't set, so plain-English requests are off.";
        return reply;
    }
    json body = {
        {"model", model},
        {"max_tokens", max_tokens},
        {"system", json::array({{{"type", "text"}, {"text", system}, {"cache_control", {{"type", "ephemeral"}}}}})},
        {"messages", json::array({{{"role", "user"}, {"content", user_text}}})},
        {"output_config", {{"effort", effort}, {"format", {{"type", "json_schema"}, {"schema", schema}}}}},
    };
    const std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);

    const auto t0 = std::chrono::steady_clock::now();
    HttpResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = http_->request("POST", "/v1/messages", payload, headers());
        if (!r.sent) {
            if (attempt < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                continue;
            }
            break;
        }
        if ((r.status == 429 || r.status == 529 || r.status >= 500) && attempt < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800 * (attempt + 1)));
            continue;
        }
        break;
    }
    reply.elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (!r.sent) {
        reply.error = "No connection: " + r.error;
        return reply;
    }
    if (r.status != 200) {
        reply.error = api_error_message(r.status, r.body);
        return reply;
    }
    json resp = json::parse(r.body, nullptr, false);
    if (!resp.is_object()) {
        reply.error = "Got an unreadable reply. Try again.";
        return reply;
    }
    if (resp.contains("model") && resp["model"].is_string()) reply.model = resp["model"].get<std::string>();
    if (resp.contains("usage") && resp["usage"].is_object()) {
        reply.input_tokens = resp["usage"].value("input_tokens", 0);
        reply.output_tokens = resp["usage"].value("output_tokens", 0);
    }
    const std::string stop = resp.contains("stop_reason") && resp["stop_reason"].is_string()
                                 ? resp["stop_reason"].get<std::string>() : "";
    if (stop == "refusal") {
        reply.refused = true;
        reply.error = "Couldn't work that one out. Try rephrasing it.";
        return reply;
    }
    if (stop == "max_tokens") {
        reply.error = "That request was too big to finish. Try a smaller one.";
        return reply;
    }
    std::string text;
    if (resp.contains("content") && resp["content"].is_array())
        for (const auto& block : resp["content"])
            if (block.is_object() && block.value("type", "") == "text" && block.contains("text") &&
                block["text"].is_string()) {
                text = block["text"].get<std::string>();
                break;
            }
    reply.result = json::parse(text, nullptr, false);
    if (!reply.result.is_object()) {
        reply.error = "Got an unreadable reply. Try again.";
        return reply;
    }
    reply.ok = true;
    return reply;
}

}  // namespace st
