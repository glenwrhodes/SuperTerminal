#include "ipc.h"

#include <windows.h>
#include <sddl.h>

#include "util.h"

using nlohmann::json;

namespace st {

namespace {
std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buf(size, '\0');
    std::wstring result;
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
            result = sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

HANDLE create_instance(const std::wstring& full_name, void* sd, bool first) {
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    return CreateNamedPipeW(full_name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, sd ? &sa : nullptr);
}
}  // namespace

IpcServer::IpcServer(Handler handler) : handler_(std::move(handler)) {
    name_ = "superterminal-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    path_ = "\\\\.\\pipe\\" + name_;
}

IpcServer::~IpcServer() {
    if (thread_.joinable()) thread_.detach();  // blocked in ConnectNamedPipe; the process is exiting
    if (security_descriptor_) LocalFree(security_descriptor_);
}

void IpcServer::stop() {}

bool IpcServer::start() {
    std::wstring sid = current_user_sid();
    if (!sid.empty()) {
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security_descriptor_,
                                                             nullptr);
    }
    const std::wstring full = widen(path_);
    HANDLE first = create_instance(full, security_descriptor_, true);
    if (first == INVALID_HANDLE_VALUE) return false;
    thread_ = std::thread([this, first, full] {
        HANDLE pipe = first;
        for (;;) {
            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected) {
                std::thread([this, pipe] { serve(reinterpret_cast<intptr_t>(pipe)); }).detach();
            } else {
                CloseHandle(pipe);
            }
            while ((pipe = create_instance(full, security_descriptor_, false)) == INVALID_HANDLE_VALUE) Sleep(200);
        }
    });
    return true;
}

void IpcServer::serve(intptr_t connection) {
    HANDLE pipe = reinterpret_cast<HANDLE>(connection);
    std::string data;
    char buf[8192];
    while (data.find('\n') == std::string::npos && data.size() < 4 * 1024 * 1024) {
        DWORD read = 0;
        if (!ReadFile(pipe, buf, sizeof buf, &read, nullptr) || read == 0) break;
        data.append(buf, read);
    }
    json reply;
    json req = json::parse(data.substr(0, data.find('\n')), nullptr, false);
    if (!req.is_object()) {
        reply = {{"kind", "error"}, {"message", "bad request"}};
    } else {
        try {
            reply = handler_(req);
        } catch (const std::exception& e) {
            reply = {{"kind", "error"}, {"message", std::string("Internal error: ") + e.what()}};
        }
    }
    std::string out = reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    DWORD written = 0;
    WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

std::string ipc_request(const std::string& name, const std::string& request_line, std::string* error) {
    std::wstring full = widen("\\\\.\\pipe\\" + name);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50; ++i) {
        h = CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(full.c_str(), 100);
    }
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "can't reach SuperTerminal (" + win32_error(GetLastError()) + ")";
        return {};
    }
    std::string line = request_line + "\n";
    DWORD n = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    std::string reply;
    char buf[8192];
    while (reply.find('\n') == std::string::npos) {
        DWORD read = 0;
        if (!ReadFile(h, buf, sizeof buf, &read, nullptr) || read == 0) break;
        reply.append(buf, read);
    }
    CloseHandle(h);
    return reply.substr(0, reply.find('\n'));
}

}  // namespace st
