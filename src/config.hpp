#pragma once
#include <string>
#include <vector>

namespace kiosk {

struct Dashboard {
    std::string name;
    std::string url;
};

struct Config {
    int port = 8787;
    // "all" (preload every dashboard) or "window:N" (current +/- N kept loaded).
    std::string preload = "all";
    // "visibility" (inactive frames laid out but not painted) or "opacity".
    std::string visual = "visibility";
    // Chromium executable; resolved via PATH. RPi OS ships "chromium" (older: "chromium-browser").
    std::string chromium = "chromium";
    std::string user_data_dir = "/var/lib/kiosk-manager/profile";
    // Extra Chromium flags appended verbatim (e.g. --ozone-platform=wayland).
    std::vector<std::string> extra_flags;
    std::vector<Dashboard> dashboards;

    // Remembered so -reload can re-read the same file.
    std::string path;
};

// Resolve which config file to use. Order: explicit (if non-empty) -> $KIOSK_MANAGER_CONFIG
// -> /etc/kiosk-manager/kiosk.conf -> ./config/kiosk.conf -> ./kiosk.conf. Returns "" if none found.
std::string resolve_config_path(const std::string &explicit_path);

// Parse the config file at cfg.path. Returns false and sets err on failure.
bool load_config(Config &cfg, std::string &err);

} // namespace kiosk
