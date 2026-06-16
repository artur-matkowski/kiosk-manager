#!/usr/bin/env bash
# Clear the kiosk browser's HTTP cache, then restart the service.
#
# Why: Chromium caches 301 (permanent) redirects forever. When a Grafana public
# dashboard is republished under a new ID and the /pogoda-* proxy redirect is
# updated, the kiosk keeps following the OLD cached 301 to a now-deleted dashboard
# ("dashboard does not exist"). Wiping the HTTP cache forces a fresh fetch.
#
# Run on the Pi:  sudo ./clear-cache.sh   (or: sudo kiosk-clear-cache if installed)
set -euo pipefail

SERVICE=kiosk-manager
CONF=/etc/kiosk-manager/kiosk.conf
PROFILE_DEFAULT=/var/lib/kiosk-manager/profile

# Needs root for systemctl + the service-owned profile under /var/lib.
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  exec sudo -- "$0" "$@"
fi

# Resolve the Chromium profile dir from config (user_data_dir), else default.
profile="$PROFILE_DEFAULT"
if [[ -r "$CONF" ]]; then
  v="$(sed -n 's/^[[:space:]]*user_data_dir[[:space:]]*=[[:space:]]*//p' "$CONF" | tail -n1)"
  [[ -n "$v" ]] && profile="$v"
fi

echo "Service: $SERVICE"
echo "Profile: $profile"

echo "==> Stopping $SERVICE"
systemctl stop "$SERVICE" || true

echo "==> Clearing browser caches"
for sub in "Default/Cache" "Default/Code Cache" "Default/GPUCache" "GrShaderCache" "ShaderCache"; do
  target="$profile/$sub"
  if [[ -e "$target" ]]; then
    echo "    rm -rf $target"
    rm -rf -- "$target"
  fi
done

echo "==> Starting $SERVICE"
systemctl start "$SERVICE"
echo "Done. Chromium will re-fetch fresh redirects on next load."
