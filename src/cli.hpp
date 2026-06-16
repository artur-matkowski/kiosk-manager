#pragma once
#include <string>
#include <vector>

namespace kiosk {

// Handle a control invocation (./kiosk-manager -next, -page N, ...). Talks to the
// running daemon over 127.0.0.1 and prints its reply. Returns a process exit code.
int run_cli(const std::vector<std::string> &args);

} // namespace kiosk
