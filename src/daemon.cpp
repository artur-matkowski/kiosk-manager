#include "daemon.hpp"

#include "httplib.h"
#include "web_assets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kiosk {
namespace {

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Best-effort recursive mkdir (ignores EEXIST and other errors).
void mkdirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur.size() > 1) ::mkdir(cur.c_str(), 0755);
        }
    }
}

// Strip the :port (and [] for IPv6) and return the bare hostname of a Host header.
std::string host_only(const std::string &host) {
    if (!host.empty() && host.front() == '[') { // [::1]:port
        auto end = host.find(']');
        return end == std::string::npos ? host : host.substr(1, end - 1);
    }
    auto colon = host.rfind(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

bool is_loopback_host(const std::string &host) {
    std::string h = host_only(host);
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

// origin = "scheme://host[:port]"; true only for a loopback origin.
bool is_local_origin(const std::string &origin) {
    auto pos = origin.find("://");
    if (pos == std::string::npos) return false;
    return is_loopback_host(origin.substr(pos + 3));
}

// One connected Server-Sent-Events stream (the kiosk page).
struct SseClient {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool closed = false;
};

class SseHub {
public:
    // The kiosk needs exactly one stream; allow a small margin for reconnect overlap.
    // Capping this prevents a local (or cross-origin) client from opening many streams
    // and exhausting httplib's worker-thread pool, which would wedge the control plane.
    static constexpr size_t kMaxClients = 4;

    // Returns nullptr if the cap is reached (caller should answer 503).
    std::shared_ptr<SseClient> add() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (clients_.size() >= kMaxClients) return nullptr;
        auto c = std::make_shared<SseClient>();
        c->queue.push_back(": connected\n\n"); // flush headers to EventSource immediately
        clients_.push_back(c);
        return c;
    }
    void remove(const std::shared_ptr<SseClient> &c) {
        {
            std::lock_guard<std::mutex> lk(c->m);
            c->closed = true;
            c->cv.notify_all();
        }
        std::lock_guard<std::mutex> lk(mtx_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
    }
    void broadcast(const std::string &msg) {
        for (auto &c : snapshot()) {
            std::lock_guard<std::mutex> lk(c->m);
            c->queue.push_back(msg);
            c->cv.notify_all();
        }
    }
    void shutdown() {
        for (auto &c : snapshot()) {
            std::lock_guard<std::mutex> lk(c->m);
            c->closed = true;
            c->cv.notify_all();
        }
    }

private:
    std::vector<std::shared_ptr<SseClient>> snapshot() {
        std::lock_guard<std::mutex> lk(mtx_);
        return clients_;
    }
    std::mutex mtx_;
    std::vector<std::shared_ptr<SseClient>> clients_;
};

class Daemon {
public:
    explicit Daemon(Config cfg) : cfg_(std::move(cfg)) {}
    int run();

private:
    std::string config_json();
    std::string status_text();
    std::string list_text();
    bool chromium_up() const { return chromium_pid_.load() > 0; }

    void setup_routes();
    void supervise_chromium();
    void kill_chromium();

    Config cfg_;
    std::mutex mtx_; // guards cfg_ and active_
    size_t active_ = 0;

    std::atomic<bool> stop_{false};
    std::atomic<pid_t> chromium_pid_{-1};
    std::atomic<bool> listen_done_{false};
    std::atomic<bool> listen_ok_{false};

    httplib::Server svr_;
    SseHub hub_;
};

std::string Daemon::config_json() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string j = "{";
    j += "\"active\":" + std::to_string(active_);
    j += ",\"visual\":\"" + json_escape(cfg_.visual) + "\"";
    j += ",\"preload\":\"" + json_escape(cfg_.preload) + "\"";
    j += ",\"dashboards\":[";
    for (size_t i = 0; i < cfg_.dashboards.size(); ++i) {
        if (i) j += ",";
        j += "{\"name\":\"" + json_escape(cfg_.dashboards[i].name) + "\",";
        j += "\"url\":\"" + json_escape(cfg_.dashboards[i].url) + "\"}";
    }
    j += "]}";
    return j;
}

std::string Daemon::status_text() {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = cfg_.dashboards.size();
    std::string name = active_ < n ? cfg_.dashboards[active_].name : "?";
    std::string s = "chromium: " + std::string(chromium_up() ? "up" : "down") + "\n";
    s += "active: " + std::to_string(active_ + 1) + "/" + std::to_string(n) +
         "  \"" + name + "\"\n";
    return s;
}

std::string Daemon::list_text() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string s = "chromium: " + std::string(chromium_up() ? "up" : "down") + "\n";
    s += "preload: " + cfg_.preload + "   visual: " + cfg_.visual +
         "   port: " + std::to_string(cfg_.port) + "\n";
    s += "dashboards (" + std::to_string(cfg_.dashboards.size()) + "):\n";
    for (size_t i = 0; i < cfg_.dashboards.size(); ++i) {
        s += (i == active_ ? "> " : "  ");
        char idx[32];
        std::snprintf(idx, sizeof(idx), "%2zu  ", i + 1);
        s += idx + cfg_.dashboards[i].name + "\n";
    }
    return s;
}

void Daemon::setup_routes() {
    // Security guard for every request. The server is loopback-bound, but the kiosk
    // browser loads cross-origin Grafana dashboards in iframes, so the browser is the
    // real attack channel. No legitimate request to this server is cross-origin.
    svr_.set_pre_routing_handler(
        [](const httplib::Request &req, httplib::Response &res) {
            // Reject non-loopback Host headers to defeat DNS-rebinding (attacker.com -> 127.0.0.1).
            if (!is_loopback_host(req.get_header_value("Host"))) {
                res.status = 421; // Misdirected Request
                res.set_content("bad host\n", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            // Reject any cross-origin request (CSRF on control endpoints, cross-origin
            // /events DoS). Same-origin GETs and the CLI send no Origin header.
            if (req.has_header("Origin") && !is_local_origin(req.get_header_value("Origin"))) {
                res.status = 403;
                res.set_content("cross-origin request rejected\n", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    svr_.Get("/healthz", [](const httplib::Request &, httplib::Response &res) {
        res.set_content("ok", "text/plain");
    });

    svr_.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(kIndexHtml, "text/html; charset=utf-8");
    });

    svr_.Get("/config", [this](const httplib::Request &, httplib::Response &res) {
        res.set_content(config_json(), "application/json");
    });

    svr_.Get("/events", [this](const httplib::Request &, httplib::Response &res) {
        auto client = hub_.add();
        if (!client) { // SSE client cap reached
            res.status = 503;
            res.set_content("too many event streams\n", "text/plain");
            return;
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [client](size_t, httplib::DataSink &sink) -> bool {
                std::string out;
                {
                    std::unique_lock<std::mutex> lk(client->m);
                    if (client->queue.empty() && !client->closed) {
                        client->cv.wait_for(lk, std::chrono::seconds(10));
                    }
                    if (client->closed) return false;
                    if (client->queue.empty()) {
                        out = ": ping\n\n"; // keep-alive / dead-peer detection
                    } else {
                        while (!client->queue.empty()) {
                            out += client->queue.front();
                            client->queue.pop_front();
                        }
                    }
                }
                return sink.write(out.data(), out.size());
            },
            [this, client](bool) { hub_.remove(client); });
    });

    auto switch_to = [this](size_t i) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            active_ = i;
        }
        hub_.broadcast("event: switch\ndata: " + std::to_string(i) + "\n\n");
    };

    svr_.Post("/control/next", [this, switch_to](const httplib::Request &, httplib::Response &res) {
        size_t n, cur;
        { std::lock_guard<std::mutex> lk(mtx_); n = cfg_.dashboards.size(); cur = active_; }
        if (n) switch_to((cur + 1) % n);
        res.set_content(status_text(), "text/plain");
    });

    svr_.Post("/control/prev", [this, switch_to](const httplib::Request &, httplib::Response &res) {
        size_t n, cur;
        { std::lock_guard<std::mutex> lk(mtx_); n = cfg_.dashboards.size(); cur = active_; }
        if (n) switch_to((cur + n - 1) % n);
        res.set_content(status_text(), "text/plain");
    });

    svr_.Post(R"(/control/page/(\d+))", [this, switch_to](const httplib::Request &req, httplib::Response &res) {
        size_t n;
        { std::lock_guard<std::mutex> lk(mtx_); n = cfg_.dashboards.size(); }
        long page = std::strtol(req.matches[1].str().c_str(), nullptr, 10); // 1-based
        if (page < 1 || static_cast<size_t>(page) > n) {
            res.status = 400;
            res.set_content("error: page out of range 1.." + std::to_string(n) + "\n", "text/plain");
            return;
        }
        switch_to(static_cast<size_t>(page - 1));
        res.set_content(status_text(), "text/plain");
    });

    svr_.Post("/control/reload", [this](const httplib::Request &, httplib::Response &res) {
        Config nc = cfg_; // keeps path
        std::string err;
        if (!load_config(nc, err)) {
            res.status = 400;
            res.set_content("reload failed: " + err + "\n", "text/plain");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cfg_ = std::move(nc);
            if (active_ >= cfg_.dashboards.size()) active_ = 0;
        }
        hub_.broadcast("event: reload\ndata: 1\n\n");
        res.set_content("reloaded\n" + status_text(), "text/plain");
    });

    svr_.Get("/control/status", [this](const httplib::Request &, httplib::Response &res) {
        res.set_content(status_text(), "text/plain");
    });

    svr_.Get("/control/list", [this](const httplib::Request &, httplib::Response &res) {
        res.set_content(list_text(), "text/plain");
    });
}

void Daemon::kill_chromium() {
    pid_t pid = chromium_pid_.load();
    if (pid <= 0) return;
    if (::kill(-pid, SIGTERM) != 0) // whole process group
        std::fprintf(stderr, "kiosk-manager: SIGTERM to group %d failed: %s\n",
                     pid, std::strerror(errno));
    for (int i = 0; i < 20 && chromium_pid_.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // grace: up to ~1s
    }
    pid = chromium_pid_.load();
    if (pid > 0) {
        std::fprintf(stderr, "kiosk-manager: chromium did not exit on SIGTERM; sending SIGKILL\n");
        ::kill(-pid, SIGKILL);
    }
}

void Daemon::supervise_chromium() {
    int backoff_s = 3; // relaunch delay; grows if Chromium keeps exiting almost immediately
    while (!stop_) {
        std::string url = "http://127.0.0.1:" + std::to_string(cfg_.port) + "/";
        std::string udd = cfg_.user_data_dir;
        std::string chromium = cfg_.chromium;
        std::vector<std::string> extra = cfg_.extra_flags;
        mkdirs(udd);
        ::chmod(udd.c_str(), 0700); // profile holds session data; keep it private to this user

        std::vector<std::string> args = {
            chromium,
            "--user-data-dir=" + udd,
            "--kiosk",
            "--app=" + url,
            "--noerrdialogs",
            "--disable-infobars",
            "--disable-session-crashed-bubble",
            "--disable-features=TranslateUI",
            "--no-first-run",
            "--disable-background-timer-throttling",
            "--disable-renderer-backgrounding",
            "--disable-backgrounding-occluded-windows",
            "--autoplay-policy=no-user-gesture-required",
        };
        for (auto &f : extra) args.push_back(f);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        auto launched_at = std::chrono::steady_clock::now();
        pid_t pid = ::fork();
        if (pid == 0) {
            setpgid(0, 0); // own process group so we can signal the whole tree
            // The daemon blocks SIGTERM/SIGINT and ignores SIGPIPE; restore defaults
            // in the child so Chromium receives SIGTERM and shuts down gracefully.
            sigset_t empty;
            sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, nullptr);
            ::signal(SIGPIPE, SIG_DFL);
            ::execvp(chromium.c_str(), argv.data());
            // Only async-signal-safe calls are allowed between fork() and exec() in a
            // multithreaded process, so use write(2) rather than fprintf here.
            const char msg[] = "kiosk-manager: failed to exec chromium\n";
            (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(127);
        }
        if (pid < 0) {
            std::fprintf(stderr, "kiosk-manager: fork failed: %s\n", std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        chromium_pid_.store(pid);
        std::fprintf(stderr, "kiosk-manager: launched chromium (pid %d) -> %s\n", pid, url.c_str());

        int status = 0;
        ::waitpid(pid, &status, 0);
        chromium_pid_.store(-1);
        if (stop_) break;

        long ran_s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - launched_at).count());

        // A healthy kiosk runs for a long time. A near-instant exit means something
        // is persistently wrong — most often another instance or a stale Chromium
        // already owns this profile, so Chromium hands off the URL and exits 0. Back
        // off exponentially instead of relaunching every 3s, which would peg the Pi.
        bool fast_exit = ran_s < 8;
        int delay_s = fast_exit ? backoff_s : 3;
        if (fast_exit)
            std::fprintf(stderr,
                         "kiosk-manager: chromium exited (status %d) after %lds; relaunching in %ds "
                         "(fast exit — another instance or a stale chromium may hold %s)\n",
                         status, ran_s, delay_s, udd.c_str());
        else
            std::fprintf(stderr,
                         "kiosk-manager: chromium exited (status %d) after %lds; relaunching in %ds\n",
                         status, ran_s, delay_s);

        for (int i = 0; i < delay_s * 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        backoff_s = fast_exit ? std::min(backoff_s * 2, 60) : 3;
    }
}

int Daemon::run() {
    // Single-instance guard: exactly one daemon may drive a given Chromium profile.
    // The listen port is NOT a guard — httplib enables SO_REUSEPORT, so a second
    // daemon would happily bind the same port and then fight over the profile,
    // making Chromium hand off the URL and exit immediately in an endless relaunch
    // storm. Take an exclusive lock on the profile before binding anything.
    mkdirs(cfg_.user_data_dir);
    const std::string lockpath = cfg_.user_data_dir + "/instance.lock";
    int lock_fd = ::open(lockpath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        std::fprintf(stderr, "kiosk-manager: cannot open lock file %s: %s (continuing)\n",
                     lockpath.c_str(), std::strerror(errno));
    } else if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            std::fprintf(stderr,
                         "kiosk-manager: another daemon is already using profile %s; exiting\n",
                         cfg_.user_data_dir.c_str());
            ::close(lock_fd);
            return 1;
        }
        std::fprintf(stderr, "kiosk-manager: flock(%s) failed: %s (continuing)\n",
                     lockpath.c_str(), std::strerror(errno));
    } else if (::ftruncate(lock_fd, 0) == 0) { // record our pid for diagnostics
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(::getpid()));
        if (n > 0) (void)::write(lock_fd, buf, static_cast<size_t>(n));
    }
    // lock_fd is intentionally left open for the lifetime of the process; the lock
    // is released automatically on exit.

    // Route SIGTERM/SIGINT to a dedicated thread; ignore SIGPIPE (closed SSE sockets).
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    setup_routes();

    std::thread server_thread([this]() {
        bool ok = svr_.listen("127.0.0.1", cfg_.port);
        listen_ok_.store(ok);
        listen_done_.store(true);
    });

    // Wait for the listener to come up (or fail to bind), without using
    // wait_until_ready() which can spin forever on a bind failure.
    auto t0 = std::chrono::steady_clock::now();
    while (!svr_.is_running()) {
        if (listen_done_.load()) {
            std::fprintf(stderr, "kiosk-manager: failed to bind 127.0.0.1:%d (port in use?)\n", cfg_.port);
            if (server_thread.joinable()) server_thread.join();
            return 1;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
            std::fprintf(stderr, "kiosk-manager: server did not become ready\n");
            svr_.stop();
            if (server_thread.joinable()) server_thread.join();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::fprintf(stderr, "kiosk-manager: serving %zu dashboards on http://127.0.0.1:%d\n",
                     cfg_.dashboards.size(), cfg_.port);
    }

    // The supervisor loop only exits once stop_ is set, and stop_ is only set by
    // this signal handler, so the handler is guaranteed to have run by the time
    // supervise_chromium() returns. Detaching avoids any join/native_handle hazard.
    std::thread sig_thread([this, &set]() {
        int sig = 0;
        sigwait(&set, &sig);
        std::fprintf(stderr, "kiosk-manager: signal %d, shutting down\n", sig);
        stop_.store(true);
        kill_chromium();
    });
    sig_thread.detach();

    supervise_chromium(); // blocks until a signal sets stop_ and chromium is reaped

    hub_.shutdown(); // wake SSE providers so worker threads don't block stop()
    svr_.stop();     // unblock listen()
    if (server_thread.joinable()) server_thread.join();
    return 0;
}

} // namespace

int run_daemon(Config cfg) {
    Daemon d(std::move(cfg));
    return d.run();
}

} // namespace kiosk
