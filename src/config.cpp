#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace kiosk {

namespace {

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool file_exists(const std::string &p) {
    struct stat st{};
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> split_ws(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

} // namespace

std::string resolve_config_path(const std::string &explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    if (const char *env = std::getenv("KIOSK_MANAGER_CONFIG"); env && *env) return env;
    for (const char *cand : {"/etc/kiosk-manager/kiosk.conf", "config/kiosk.conf", "kiosk.conf"}) {
        if (file_exists(cand)) return cand;
    }
    return "";
}

bool load_config(Config &cfg, std::string &err) {
    if (cfg.path.empty()) {
        err = "no config file found (looked in /etc/kiosk-manager/kiosk.conf, ./config/kiosk.conf, ./kiosk.conf)";
        return false;
    }
    std::ifstream in(cfg.path);
    if (!in) {
        err = "cannot open config file: " + cfg.path;
        return false;
    }

    // Reset the bits that come from the file; keep cfg.path.
    cfg.dashboards.clear();
    cfg.extra_flags.clear();

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        // A dashboard line is "Name | URL"; settings are "key = value".
        // Test for '|' first because URLs legitimately contain '='.
        auto bar = t.find('|');
        if (bar != std::string::npos) {
            Dashboard d;
            d.name = trim(t.substr(0, bar));
            d.url = trim(t.substr(bar + 1));
            if (d.name.empty() || d.url.empty()) {
                err = cfg.path + ":" + std::to_string(lineno) + ": dashboard needs 'Name | URL'";
                return false;
            }
            // Only http(s) URLs — an iframe src of javascript:/data: would run script
            // in the kiosk page's own (privileged, loopback) origin.
            if (d.url.rfind("http://", 0) != 0 && d.url.rfind("https://", 0) != 0) {
                err = cfg.path + ":" + std::to_string(lineno) + ": URL must start with http:// or https://";
                return false;
            }
            cfg.dashboards.push_back(std::move(d));
            continue;
        }

        auto eq = t.find('=');
        if (eq == std::string::npos) {
            err = cfg.path + ":" + std::to_string(lineno) + ": not a setting or dashboard: " + t;
            return false;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key == "port") {
            cfg.port = std::atoi(val.c_str());
            if (cfg.port <= 0 || cfg.port > 65535) {
                err = cfg.path + ":" + std::to_string(lineno) + ": invalid port";
                return false;
            }
        } else if (key == "preload") {
            cfg.preload = val;
        } else if (key == "visual") {
            cfg.visual = val;
        } else if (key == "chromium") {
            cfg.chromium = val;
        } else if (key == "user_data_dir") {
            cfg.user_data_dir = val;
        } else if (key == "extra_flags") {
            for (auto &f : split_ws(val)) cfg.extra_flags.push_back(f);
        } else {
            err = cfg.path + ":" + std::to_string(lineno) + ": unknown setting '" + key + "'";
            return false;
        }
    }

    if (cfg.dashboards.empty()) {
        err = cfg.path + ": no dashboards defined (add lines like 'Name | http://...')";
        return false;
    }
    return true;
}

} // namespace kiosk
