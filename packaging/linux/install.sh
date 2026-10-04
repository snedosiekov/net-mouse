#!/bin/sh
# Builds (if needed) and installs net-mouse-daemon as a systemd service.
#
#   sudo packaging/linux/install.sh master|slave [--monitors 1920x1080+0+0,...]
#   sudo packaging/linux/install.sh --uninstall
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build-linux"
CONFIG_DIR=/etc/net-mouse
CONFIG="$CONFIG_DIR/netmouse.ini"
UNIT=/etc/systemd/system/net-mouse.service

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"

set_key() {  # set_key KEY VALUE: replace or append KEY=VALUE in the config
  if grep -q "^$1=" "$CONFIG" 2>/dev/null; then
    sed -i "s|^$1=.*|$1=$2|" "$CONFIG"
  else
    echo "$1=$2" >> "$CONFIG"
  fi
}

firewall() {  # firewall allow|remove
  if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    if [ "$1" = allow ]; then ufw allow 47800/udp comment net-mouse; else ufw delete allow 47800/udp || true; fi
  elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    if [ "$1" = allow ]; then firewall-cmd --permanent --add-port=47800/udp; else firewall-cmd --permanent --remove-port=47800/udp || true; fi
    firewall-cmd --reload
  fi
}

if [ "${1:-}" = "--uninstall" ]; then
  systemctl disable --now net-mouse.service 2>/dev/null || true
  rm -f "$UNIT" /usr/local/bin/net-mouse-daemon /etc/modules-load.d/net-mouse.conf
  systemctl daemon-reload
  firewall remove
  echo "Uninstalled. Settings kept in $CONFIG_DIR."
  exit 0
fi

ROLE=${1:-}
[ "$ROLE" = master ] || [ "$ROLE" = slave ] || die "usage: $0 master|slave [--monitors SPEC] | --uninstall"
shift
MONITORS=""
if [ "${1:-}" = "--monitors" ]; then MONITORS=${2:?missing monitor spec}; fi

if [ ! -x "$BUILD/net-mouse-daemon" ]; then
  command -v cmake >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1 || die "install cmake and g++ (C++20) first"
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" --target net-mouse-daemon -j"$(nproc)"
fi

systemctl stop net-mouse.service 2>/dev/null || true
install -Dm755 "$BUILD/net-mouse-daemon" /usr/local/bin/net-mouse-daemon
install -d -m755 "$CONFIG_DIR"
touch "$CONFIG"
set_key role "$ROLE"
[ -z "$MONITORS" ] || set_key monitors "$MONITORS"
install -Dm644 "$ROOT/packaging/linux/net-mouse.service" "$UNIT"
echo uinput > /etc/modules-load.d/net-mouse.conf
modprobe uinput
firewall allow
systemctl daemon-reload
systemctl enable --now net-mouse.service

echo
echo "Installed as $ROLE. Detected hardware:"
/usr/local/bin/net-mouse-daemon --config "$CONFIG" --list-devices
echo
echo "Logs: journalctl -u net-mouse -f"
