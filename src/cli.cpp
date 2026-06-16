#include "cli.hpp"

#include "config.hpp"
#include "httplib.h"

#include <cstdio>
#include <string>

namespace kiosk {
namespace {

int resolve_port(const std::string &config_override, int port_override) {
    if (port_override > 0) return port_override;
    Config cfg;
    cfg.path = resolve_config_path(config_override);
    std::string err;
    if (!cfg.path.empty() && load_config(cfg, err)) return cfg.port;
    return 8787; // default if no readable config
}

void usage() {
    std::fprintf(stderr,
        "usage: kiosk-manager <command>\n"
        "  -next                 show the next dashboard\n"
        "  -prev                 show the previous dashboard\n"
        "  -page N               show dashboard N (1-based)\n"
        "  -status               print current state\n"
        "  -list                 list configured dashboards\n"
        "  -reload               re-read the config file\n"
        "  -daemon               run the background service\n"
        "options: -port N, -config PATH\n");
}

} // namespace

int run_cli(const std::vector<std::string> &args) {
    std::string action;     // "next","prev","page","status","list","reload"
    std::string page_arg;
    std::string config_override;
    int port_override = 0;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-next") action = "next";
        else if (a == "-prev") action = "prev";
        else if (a == "-status") action = "status";
        else if (a == "-list") action = "list";
        else if (a == "-reload") action = "reload";
        else if (a == "-page") {
            action = "page";
            if (i + 1 < args.size()) page_arg = args[++i];
        } else if (a == "-port") {
            if (i + 1 < args.size()) port_override = std::atoi(args[++i].c_str());
        } else if (a == "-config") {
            if (i + 1 < args.size()) config_override = args[++i];
        } else {
            std::fprintf(stderr, "kiosk-manager: unknown argument '%s'\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (action.empty()) { usage(); return 2; }
    if (action == "page" && (page_arg.empty() || std::atoi(page_arg.c_str()) < 1)) {
        std::fprintf(stderr, "kiosk-manager: -page needs a positive number\n");
        return 2;
    }

    int port = resolve_port(config_override, port_override);
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    httplib::Result res(nullptr, httplib::Error::Unknown);
    if (action == "status") res = cli.Get("/control/status");
    else if (action == "list") res = cli.Get("/control/list");
    else if (action == "next") res = cli.Post("/control/next", "", "text/plain");
    else if (action == "prev") res = cli.Post("/control/prev", "", "text/plain");
    else if (action == "reload") res = cli.Post("/control/reload", "", "text/plain");
    else if (action == "page") res = cli.Post("/control/page/" + page_arg, "", "text/plain");

    if (!res) {
        std::fprintf(stderr,
            "kiosk-manager: cannot reach the service on 127.0.0.1:%d (is it running? 'systemctl status kiosk-manager')\n",
            port);
        return 1;
    }
    std::fputs(res->body.c_str(), stdout);
    return (res->status >= 200 && res->status < 300) ? 0 : 1;
}

} // namespace kiosk
