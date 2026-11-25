#pragma once

#include "types.h"

#include <memory>

namespace humbug {
class Monitor {
public:
    explicit Monitor(Options options);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    void start();
    void stop();
    // Copies the latest state and consumes pending block notifications.
    bool take(Snapshot& snapshot);
    // Commands are checked against the current group and track before execution.
    void block(BlockCommand command);
    void set_blocklists(bool christmas, bool custom);
    void set_auto_stop(bool enabled);
    void reset_custom_blocklist();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace humbug
