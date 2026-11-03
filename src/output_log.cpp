#include "output_log.h"

#include "util.h"

namespace st {

void OutputLog::append(std::string_view bytes) {
    std::lock_guard lock(mu_);
    buf_.append(bytes);
    if (buf_.size() > kCapacity) {
        size_t drop = buf_.size() - kCapacity / 2;
        buf_.erase(0, drop);
        base_ += drop;
    }
}

void OutputLog::mark_command_start() {
    std::lock_guard lock(mu_);
    mark_ = base_ + buf_.size();
}

std::string OutputLog::since_last_command(size_t max_bytes) const {
    std::string raw;
    {
        std::lock_guard lock(mu_);
        uint64_t from = mark_ < base_ ? base_ : mark_;
        raw = buf_.substr(static_cast<size_t>(from - base_));
    }
    return tail(strip_vt(raw), max_bytes);
}

}  // namespace st
