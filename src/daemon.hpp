#pragma once
#include "config.hpp"

namespace kiosk {

// Run the background service: serve the SPA + control API on 127.0.0.1:cfg.port,
// and launch/supervise Chromium in kiosk mode. Returns a process exit code.
int run_daemon(Config cfg);

} // namespace kiosk
