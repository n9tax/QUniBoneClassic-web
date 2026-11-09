#!/bin/bash
# install.sh - set up the QBone/UniBone control panel on the BeagleBone.
#
#   sudo ./install.sh             install systemd service, qbmenu command,
#                                 and show the menu on interactive logins
#   sudo ./install.sh --no-menu   same, but do not touch ~/.profile
#   sudo ./install.sh --uninstall remove service, commands and login hook
#
# Run it as root (the QUniBone software lives in root's home directory).

set -e

CONTROL_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
QUNIBONE_HOME="$(dirname "$CONTROL_DIR")"
UNIT=/etc/systemd/system/qbcontrol.service
PROFILE="$QUNIBONE_HOME/.profile"
MARK_BEGIN="# >>> qbcontrol menu >>>"
MARK_END="# <<< qbcontrol menu <<<"

if [ "$(id -u)" != "0" ]; then
    echo "Please run as root: sudo $0 $*"
    exit 1
fi

remove_profile_hook() {
    if [ -f "$PROFILE" ] && grep -q "$MARK_BEGIN" "$PROFILE"; then
        sed -i "/$MARK_BEGIN/,/$MARK_END/d" "$PROFILE"
        echo "Removed login menu from $PROFILE"
    fi
}

if [ "$1" == "--uninstall" ]; then
    systemctl disable --now qbcontrol 2>/dev/null || true
    rm -f "$UNIT" /usr/local/bin/qbmenu /usr/local/bin/qbcontrold
    systemctl daemon-reload
    remove_profile_hook
    echo "Uninstalled. Settings and configurations are kept in $QUNIBONE_HOME/.qbcontrol"
    exit 0
fi

# --- Python 3 (Debian 8 images ship 3.4; standard library is enough)
if ! command -v python3 >/dev/null; then
    echo "python3 not found, installing ..."
    apt-get install --yes python3
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 4) else 1)' || {
    echo "Python 3.4 or newer is needed."; exit 1; }

chmod +x "$CONTROL_DIR/qbcontrold" "$CONTROL_DIR/qbmenu" "$CONTROL_DIR/tools/fake_demo.py"

# --- warn about the old screen-based autostart: two demos would fight over the PRUs
if [ -f "$PROFILE" ] && grep -v '^\s*#' "$PROFILE" | grep -q 'autostart\.sh'; then
    echo
    echo "WARNING: $PROFILE still starts ./autostart.sh in a screen session (tty1)."
    echo "         The control daemon now does the autostart. Only one program may"
    echo "         drive the bus: comment out the 'screen -dmS \"QUniBone\" ./autostart.sh'"
    echo "         line in $PROFILE, then set up autostart in the web panel or qbmenu."
    echo
fi

# --- systemd service
sed "s|@CONTROL_DIR@|$CONTROL_DIR|g" "$CONTROL_DIR/qbcontrol.service" > "$UNIT"
systemctl daemon-reload
systemctl enable qbcontrol
systemctl restart qbcontrol
echo "Service qbcontrol installed and started."

# --- commands
ln -sf "$CONTROL_DIR/qbmenu" /usr/local/bin/qbmenu
ln -sf "$CONTROL_DIR/qbcontrold" /usr/local/bin/qbcontrold
echo "Commands qbmenu and qbcontrold linked into /usr/local/bin."

# --- show the menu on interactive logins (ssh, serial console)
if [ "$1" != "--no-menu" ]; then
    remove_profile_hook
    cat >> "$PROFILE" <<EOF
$MARK_BEGIN
# Show the QBone/UniBone control menu on interactive logins.
# "X" in the menu leaves to the shell. Skip it with: ssh -t host QBC_NOMENU=1 bash -l
if [ -t 0 ] && [ -z "\$QBC_NOMENU" ] && [ -z "\$STY" ] && [ -x /usr/local/bin/qbmenu ]; then
    /usr/local/bin/qbmenu
fi
$MARK_END
EOF
    echo "Login menu added to $PROFILE"
fi

sleep 1
IP=$(hostname -I 2>/dev/null | awk '{print $1}')
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("http_port",1170))' \
       "$QUNIBONE_HOME/.qbcontrol/settings.json" 2>/dev/null || echo 1170)
echo
echo "Web panel:     http://${IP:-<beaglebone>}:$PORT/"
echo "Terminal menu: qbmenu"
