#include "cli.hpp"
#include "config.hpp"
#include "daemon.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kiosk;

namespace {

// Accept GNU-style "--flag" as an alias for this tool's single-dash "-flag"
// convention, so e.g. "--next" works like "-next".
std::string normalize_flag(const std::string &a) {
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') return a.substr(1);
    return a;
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
        "  kiosk-manager -reload            re-read the config file\n"
        "  kiosk-manager -refresh           reload all dashboard pages (like F5)\n\n"
        "Options: -config PATH, -port N\n");
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) args.push_back(normalize_flag(argv[i]));

    bool daemon = false, help = false;
    std::string config_override;
    int port_override = 0;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-daemon" || a == "serve") daemon = true;
        else if (a == "-h" || a == "-help") help = true;
        else if (a == "-config") { if (i + 1 < args.size()) config_override = args[++i]; }
        else if (a == "-port") { if (i + 1 < args.size()) port_override = std::atoi(args[++i].c_str()); }
        // Control flags and any unknown tokens are validated by run_cli() below.
    }

    if (help) { print_help(); return 0; }
    if (args.empty()) { print_help(); return 0; } // bare run: show usage, never auto-start a daemon

    // The background service runs ONLY when explicitly requested with -daemon/serve.
    // Anything else — a control flag, or a typo like "--bogus" — routes to the CLI
    // client, which validates arguments and errors on unknown ones. This stops a
    // mistyped command from silently launching a second, competing daemon.
    if (!daemon) return run_cli(args);

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
