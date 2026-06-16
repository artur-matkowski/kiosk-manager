# kiosk-manager

A tiny background service for a Raspberry Pi Grafana kiosk that switches between
public dashboards **instantly** — no Grafana reload on switch — controlled from a
single binary:

```sh
kiosk-manager -next        # show the next dashboard
kiosk-manager -page 2      # jump to dashboard 2
kiosk-manager -prev
kiosk-manager -status
kiosk-manager -list
kiosk-manager -reload      # re-read the config (e.g. after adding a dashboard)
```

## How it works

Reloading Grafana is the slow part of a kiosk (full page paint + JS bootstrap +
datasource queries). kiosk-manager avoids it entirely:

- One Chromium window (`--kiosk`) loads a small local page served by the service.
- That page embeds **every** dashboard as a preloaded `<iframe>` (Grafana public
  dashboards are iframe-embeddable) and just changes **which one is visible**.
- A switch is a single CSS/compositor change — no network, no reload, sub-frame fast.
- Chromium is launched with the no-throttle flags
  (`--disable-background-timer-throttling`, `--disable-renderer-backgrounding`,
  `--disable-backgrounding-occluded-windows`) so background dashboards keep
  refreshing and a freshly-shown one is already up to date.

The same binary is both the **service** (`-daemon`, what systemd runs) and the
**CLI** (`-next`, `-page N`, …). CLI commands are localhost HTTP requests to the
running service; the service holds the dashboard list + current index and pushes
switches to the page over Server-Sent Events.

```
 kiosk-manager -next ──HTTP──▶ kiosk-manager -daemon ──SSE──▶ Chromium SPA ──▶ flips iframe
                                  (systemd service)            (--kiosk)
```

## Requirements

- A C++17 toolchain + CMake (`sudo apt install build-essential cmake`).
- Chromium (`sudo apt install chromium` — older images: `chromium-browser`).
- **Grafana must allow framing.** Public dashboards are embeddable, but the Grafana
  instance needs `allow_embedding = true` (under `[security]`) and public dashboard
  sharing enabled. The instance here is your own, so this is just a settings check.

The only third-party dependency, [cpp-httplib](https://github.com/yhirose/cpp-httplib),
is vendored as a single header in `third_party/`. No network access is needed to build.

## Install (Raspberry Pi)

```sh
git clone <this repo> kiosk-manager && cd kiosk-manager
sudo ./install.sh
sudoedit /etc/kiosk-manager/kiosk.conf   # add your dashboards
sudo systemctl start kiosk-manager
```

`install.sh` builds, installs the binary to `/usr/local/bin`, drops the config in
`/etc/kiosk-manager/kiosk.conf`, and installs a systemd unit wired to the user that
ran it. Building on the Pi itself is simplest; cross-compiling for aarch64 also works.

### Manual build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/kiosk-manager -daemon -config config/kiosk.conf   # foreground, for testing
```

## Configuration

`/etc/kiosk-manager/kiosk.conf` — `key = value` settings plus `Name | URL` dashboards:

```ini
port = 8787
preload = all          # or window:1  (current ±1 loaded, rest lazy)
visual = visibility    # or opacity
chromium = chromium
user_data_dir = /var/lib/kiosk-manager/profile
# extra_flags = --ozone-platform=wayland

Solar   | https://grafana.example.com/public-dashboards/your-dashboard-id
Network | https://grafana.example.com/public-dashboards/another-dashboard-id
```

Dashboard order defines the `-page N` numbers (1-based) and `-next`/`-prev` cycling.
`-reload` re-reads this file and rebuilds the page live (changing `port` needs a
service restart).

### Speed vs. memory (Pi 4 / 4 GB)

- `preload = all` (default) keeps every dashboard loaded → switches are instant but
  6 live Grafana dashboards are RAM/GPU heavy. Check headroom with `free -h`.
- If memory is tight, set `preload = window:1`: only the current dashboard and its
  neighbors stay loaded; a first visit to a cold dashboard reloads, neighbors are instant.
- `visual = visibility` (default) keeps inactive dashboards laid out but unpainted
  (lower continuous GPU load). `visual = opacity` keeps them painted for a truly
  zero-cost flip at higher steady GPU cost — use only if the Pi has headroom.

## systemd

The service must run inside the graphical session. The shipped unit defaults to X11:

```ini
Environment=DISPLAY=:0
Environment=XAUTHORITY=/home/<user>/.Xauthority
```

For Wayland (e.g. labwc on newer Raspberry Pi OS), swap those for:

```ini
Environment=WAYLAND_DISPLAY=wayland-0
Environment=XDG_RUNTIME_DIR=/run/user/1000
```

and add `extra_flags = --ozone-platform=wayland` to the config. Common commands:

```sh
sudo systemctl restart kiosk-manager   # cleanly cycles the whole kiosk
journalctl -u kiosk-manager -f         # logs (launches, crashes, relaunches)
```

The service owns Chromium: it relaunches it if it crashes and shuts it down
gracefully (SIGTERM, SIGKILL fallback) when the service stops.

## Troubleshooting

- **Blank/refused frames:** Grafana framing not allowed — set `allow_embedding = true`
  and confirm the public-dashboard URLs load directly in a browser.
- **`cannot reach the service ...`** from a CLI command: the service isn't running —
  `sudo systemctl status kiosk-manager` / `journalctl -u kiosk-manager`.
- **Chromium won't start:** check the binary name (`chromium` vs `chromium-browser`)
  in the config, and that `DISPLAY`/Wayland env in the unit matches the session.
- **Nothing shows but logs say "launched chromium":** display/session env mismatch in
  the unit (wrong `DISPLAY`, `XAUTHORITY`, or X11-vs-Wayland).
