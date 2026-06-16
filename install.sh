#!/usr/bin/env bash
# Build and install kiosk-manager as a systemd service on a Raspberry Pi.
# Run with sudo from the project root:  sudo ./install.sh
set -euo pipefail

PREFIX=/usr/local/bin
CONF_DIR=/etc/kiosk-manager
UNIT=/etc/systemd/system/kiosk-manager.service

# Resolve the desktop user whose graphical session shows the kiosk.
TARGET_USER="${SUDO_USER:-$(id -un)}"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
TARGET_UID="$(id -u "$TARGET_USER")"

# Escape characters that are special on sed's replacement side (delimiter is '|').
sed_escape() { printf '%s' "$1" | sed -e 's/[&|\\]/\\&/g'; }

cd "$(dirname "$0")"

echo "==> Building (Release)"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"

echo "==> Installing binary to $PREFIX/kiosk-manager"
install -m 0755 build/kiosk-manager "$PREFIX/kiosk-manager"

echo "==> Installing $PREFIX/kiosk-clear-cache"
install -m 0755 clear-cache.sh "$PREFIX/kiosk-clear-cache"

echo "==> Installing config to $CONF_DIR/kiosk.conf"
mkdir -p "$CONF_DIR"
# Config holds dashboard URLs (with sharing tokens); keep it private to the kiosk user.
if [[ -f "$CONF_DIR/kiosk.conf" ]]; then
  echo "    (keeping existing $CONF_DIR/kiosk.conf; sample at $CONF_DIR/kiosk.conf.sample)"
  install -o "$TARGET_USER" -g "$TARGET_USER" -m 0644 config/kiosk.conf "$CONF_DIR/kiosk.conf.sample"
else
  install -o "$TARGET_USER" -g "$TARGET_USER" -m 0600 config/kiosk.conf "$CONF_DIR/kiosk.conf"
fi

echo "==> Installing systemd unit for user '$TARGET_USER'"
U_ESC="$(sed_escape "$TARGET_USER")"
H_ESC="$(sed_escape "$TARGET_HOME")"
sed -e "s|^User=.*|User=$U_ESC|" \
    -e "s|^Environment=XAUTHORITY=.*|Environment=XAUTHORITY=$H_ESC/.Xauthority|" \
    -e "s|XDG_RUNTIME_DIR=/run/user/1000|XDG_RUNTIME_DIR=/run/user/$TARGET_UID|" \
    systemd/kiosk-manager.service > "$UNIT"
chmod 0644 "$UNIT"

systemctl daemon-reload
systemctl enable kiosk-manager.service

echo
echo "Done. Next steps:"
echo "  1. Edit your dashboards:   sudoedit $CONF_DIR/kiosk.conf"
echo "  2. Start the kiosk:        sudo systemctl start kiosk-manager"
echo "  3. Switch dashboards:      kiosk-manager -next   |   kiosk-manager -page 2"
echo
echo "If the Pi uses Wayland (not X11), edit $UNIT per the comments and set"
echo "  'extra_flags = --ozone-platform=wayland' in $CONF_DIR/kiosk.conf."
