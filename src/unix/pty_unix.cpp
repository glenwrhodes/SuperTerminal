#include "pty_host.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
// forkpty lives in the system <util.h>, which our own src/util.h shadows on the include path.
struct termios;
struct winsize;
extern "C" int forkpty(int*, char*, struct termios*, struct winsize*);
#else
#include <pty.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "output_log.h"

namespace st {

namespace {
std::atomic<bool> g_resized{false};
void on_winch(int) { g_resized = true; }

std::vector<char*> c_argv(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

void write_all(int fd, const char* data, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, data, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        data += w;
        n -= static_cast<size_t>(w);
    }
}

int exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
}  // namespace

bool has_console() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

int run_direct(const HostOptions& opt) {
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        auto args = c_argv(opt.argv);
        execvp(args[0], args.data());
        std::fprintf(stderr, "SuperTerminal: couldn't start %s: %s\n", args[0], std::strerror(errno));
        _exit(127);
    }
    // Ctrl+C belongs to the shell; it's in our process group, so ignore it here.
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return exit_code(status);
}

int run_in_pseudoconsole(const HostOptions& opt) {
    termios saved{};
    const bool tty = tcgetattr(STDIN_FILENO, &saved) == 0;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) ws = {30, 120, 0, 0};

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, tty ? &saved : nullptr, &ws);
    if (pid < 0) return -1;
    if (pid == 0) {
        auto args = c_argv(opt.argv);
        execvp(args[0], args.data());
        std::fprintf(stderr, "SuperTerminal: couldn't start %s: %s\n", args[0], std::strerror(errno));
        _exit(127);
    }

    // Our terminal goes raw: every byte goes to the shell, which has its own terminal.
    if (tty) {
        termios raw = saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    struct sigaction sa {};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, nullptr);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    std::vector<char> buf(32 * 1024);
    bool stdin_open = true;
    for (;;) {
        if (g_resized.exchange(false)) {
            winsize now{};
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &now) == 0) ioctl(master, TIOCSWINSZ, &now);
        }
        pollfd fds[2] = {{master, POLLIN, 0}, {STDIN_FILENO, static_cast<short>(stdin_open ? POLLIN : 0), 0}};
        int rc = poll(fds, 2, 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = ::read(master, buf.data(), buf.size());
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;  // EIO: the shell has exited
            }
            write_all(STDOUT_FILENO, buf.data(), static_cast<size_t>(n));
            if (opt.log) opt.log->append(std::string_view(buf.data(), static_cast<size_t>(n)));
        }
        if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                stdin_open = false;
            } else {
                write_all(master, buf.data(), static_cast<size_t>(n));
            }
        }
    }

    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    ::close(master);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return exit_code(status);
}

}  // namespace st
