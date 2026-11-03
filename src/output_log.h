#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace st {

// A rolling copy of everything PowerShell printed, so Claude can see the
// output of the last command ("why did that fail?", "fix it").
class OutputLog {
public:
    void append(std::string_view bytes);
    // Called when a command is about to run: later output belongs to it.
    void mark_command_start();
    // Plain text printed since the last mark (the last command's output,
    // followed by the current prompt line), at most max_bytes from the end.
    std::string since_last_command(size_t max_bytes) const;
    bool enabled() const { return enabled_; }
    void set_enabled(bool on) { enabled_ = on; }

private:
    static constexpr size_t kCapacity = 256 * 1024;
    mutable std::mutex mu_;
    std::string buf_;
    uint64_t base_ = 0;  // absolute offset of buf_[0]
    uint64_t mark_ = 0;  // absolute offset of the last command start
    bool enabled_ = false;
};

}  // namespace st
