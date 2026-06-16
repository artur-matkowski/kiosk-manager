#include "cli.hpp"
#include "config.hpp"
#include "daemon.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kiosk;

namespace {

bool is_control_flag(const std::string &a) {
    return a == "-next" || a == "-prev" || a == "-page" ||
           a == "-status" || a == "-list" || a == "-reload";
}

void print_help() {
    std::printf(
        "kiosk-manager — fast Grafana dashboard switcher\n\n"
        "Service (systemd ExecStart):\n"
        "  kiosk-manager -daemon            run the background service + Chromium kiosk\n\n"
        "Control (talks to the running service):\n"
        "  kiosk-manager -next              show the next dashboard\n"
        "  kiosk-manager -prev              show the previous dashboard\n"
        "  kiosk-manager -page N            show dashboard N (1-based)\n"
        "  kiosk-manager -status            print current state\n"
        "  kiosk-manager -list              list configured dashboards\n"
        "  kiosk-manager -reload            re-read the config file\n\n"
        "Options: -config PATH, -port N\n");
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    bool daemon = false, control = false, help = false;
    std::string config_override;
    int port_override = 0;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-daemon" || a == "serve") daemon = true;
        else if (is_control_flag(a)) control = true;
        else if (a == "-h" || a == "--help" || a == "-help") help = true;
        else if (a == "-config") { if (i + 1 < args.size()) config_override = args[++i]; }
        else if (a == "-port") { if (i + 1 < args.size()) port_override = std::atoi(args[++i].c_str()); }
        // Other tokens (e.g. the N after -page) are parsed by the CLI handler.
    }

    if (help) { print_help(); return 0; }

    // Control flags route to the CLI client unless -daemon was explicitly requested.
    if (control && !daemon) return run_cli(args);

    // Otherwise run as the background service.
    Config cfg;
    cfg.path = resolve_config_path(config_override);
    std::string err;
    if (!load_config(cfg, err)) {
        std::fprintf(stderr, "kiosk-manager: %s\n", err.c_str());
        return 1;
    }
    if (port_override > 0) cfg.port = port_override;
    return run_daemon(cfg);
}
