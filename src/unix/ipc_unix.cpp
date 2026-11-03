#include "ipc.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

#include "util.h"

using nlohmann::json;

namespace st {

namespace {
bool fill_address(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

std::string socket_path(const std::string& name) {
    return (temp_dir() / ("CoreFxPipe_" + name)).string();
}

void write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

std::string read_line(int fd) {
    std::string data;
    char buf[8192];
    while (data.find('\n') == std::string::npos && data.size() < 4 * 1024 * 1024) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    return data.substr(0, data.find('\n'));
}
}  // namespace

IpcServer::IpcServer(Handler handler) : handler_(std::move(handler)) {
    std::random_device rd;
    name_ = "st-" + std::to_string(getpid()) + "-" + std::to_string(rd() % 1000000);
    path_ = socket_path(name_);
}

IpcServer::~IpcServer() {
    stop();
    if (thread_.joinable()) thread_.detach();
}

void IpcServer::stop() {
    if (!path_.empty()) ::unlink(path_.c_str());
}

bool IpcServer::start() {
    sockaddr_un addr;
    if (!fill_address(path_, addr)) return false;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    ::unlink(path_.c_str());
    mode_t old = ::umask(0177);  // created as 0600: only this user can connect
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (rc < 0 || ::listen(fd, 16) < 0) {
        ::close(fd);
        return false;
    }
    ::chmod(path_.c_str(), 0600);
    listener_ = fd;
    thread_ = std::thread([this] {
        for (;;) {
            int c = ::accept(static_cast<int>(listener_), nullptr, nullptr);
            if (c < 0) {
                if (errno == EINTR) continue;
                return;
            }
            std::thread([this, c] { serve(c); }).detach();
        }
    });
    return true;
}

void IpcServer::serve(intptr_t connection) {
    int fd = static_cast<int>(connection);
    json reply;
    json req = json::parse(read_line(fd), nullptr, false);
    if (!req.is_object()) {
        reply = {{"kind", "error"}, {"message", "bad request"}};
    } else {
        try {
            reply = handler_(req);
        } catch (const std::exception& e) {
            reply = {{"kind", "error"}, {"message", std::string("Internal error: ") + e.what()}};
        }
    }
    write_all(fd, reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
    ::close(fd);
}

std::string ipc_request(const std::string& name, const std::string& request_line, std::string* error) {
    sockaddr_un addr;
    if (!fill_address(socket_path(name), addr)) {
        if (error) *error = "socket path too long";
        return {};
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        if (error) *error = std::string("can't reach SuperTerminal (") + std::strerror(errno) + ")";
        if (fd >= 0) ::close(fd);
        return {};
    }
    write_all(fd, request_line + "\n");
    std::string reply = read_line(fd);
    ::close(fd);
    return reply;
}

}  // namespace st
