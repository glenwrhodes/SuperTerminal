#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace st {

// The local channel the shell integration talks to: one JSON request line in,
// one JSON reply line out, per connection.
//   Windows: a named pipe \\.\pipe\<name>, current user + SYSTEM only, no remote clients.
//   Unix:    a socket at $TMPDIR/CoreFxPipe_<name>, mode 0600. That's the path .NET
//            uses for named pipes, so the same PowerShell code works on every OS.
class IpcServer {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

    explicit IpcServer(Handler handler);
    ~IpcServer();

    bool start();
    void stop();  // removes the socket file on Unix
    const std::string& name() const { return name_; }

private:
    void serve(intptr_t connection);

    Handler handler_;
    std::string name_;
    std::string path_;
    intptr_t listener_ = -1;
    void* security_descriptor_ = nullptr;
    std::thread thread_;
};

// Client side (used by `st --client`): sends one request line, returns the
// reply line, or "" with *error set.
std::string ipc_request(const std::string& name, const std::string& request_line, std::string* error);

}  // namespace st
