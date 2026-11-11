#!/usr/bin/env bash
# Start the Grove Vision web UI at boot, with systemd keeping it alive.
#
#   ./deploy/install.sh              install (or update) and start it
#   ./deploy/install.sh --print      only show the unit files it would install
#   ./deploy/install.sh --uninstall  stop it and remove the unit files
#
# Run it as your normal user, not with sudo. It finds Node and the project from
# your environment, and asks for your sudo password only to install the units.
#
# What gets installed:
#   grove-web.service        runs server.js as you at boot; systemd restarts it if it
#                            crashes or exits, and never gives up retrying.
#   grove-web-health.timer   every minute, asks the server for /api/health and restarts
#   grove-web-health.service the service if it fails to answer twice in a row (a hung process).
# The board itself is watched inside the server (see README: "Freeze watchdog").
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_USER="$(id -un)"
PORT="${PORT:-3000}"
HOST="${HOST:-0.0.0.0}"
UNIT_DIR=/etc/systemd/system

unit_service() {
  cat <<EOF
[Unit]
Description=Grove Vision AI V2 web UI
Documentation=file://$DIR/README.md
After=network.target
# Keep retrying forever (by default systemd gives up after 5 quick failures).
StartLimitIntervalSec=0

[Service]
Type=simple
User=$RUN_USER
Group=$(id -gn)
# Serial port access, whatever groups your login session has.
SupplementaryGroups=dialout
WorkingDirectory=$DIR
Environment=PORT=$PORT
Environment=HOST=$HOST
ExecStart=$NODE $DIR/server.js
Restart=always
RestartSec=3
# SIGINT lets the server stop the board's stream before closing the port.
KillSignal=SIGINT
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF
}

unit_health_service() {
  cat <<EOF
[Unit]
Description=Restart grove-web if its web server stops answering
After=grove-web.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'systemctl is-active --quiet grove-web.service || exit 0; \\
  for try in 1 2; do curl -fsS --max-time 10 -o /dev/null http://127.0.0.1:$PORT/api/health && exit 0; sleep 5; done; \\
  echo "grove-web did not answer twice, restarting it"; systemctl restart grove-web.service'
EOF
}

unit_health_timer() {
  cat <<EOF
[Unit]
Description=Check every minute that the Grove web UI answers

[Timer]
OnBootSec=2min
OnUnitActiveSec=1min
AccuracySec=10s

[Install]
WantedBy=timers.target
EOF
}

if [[ $EUID -eq 0 ]]; then
  echo "Run this as your normal user, without sudo. It will ask for your password when it needs it." >&2
  exit 1
fi

case "${1:-}" in
  --uninstall)
    sudo systemctl disable --now grove-web-health.timer grove-web.service 2>/dev/null || true
    sudo rm -f "$UNIT_DIR/grove-web.service" "$UNIT_DIR/grove-web-health.service" "$UNIT_DIR/grove-web-health.timer"
    sudo systemctl daemon-reload
    echo "Removed. The web UI no longer starts at boot."
    exit 0
    ;;
  --print|"") ;;
  *) echo "Unknown option: $1 (use --print or --uninstall)" >&2; exit 1 ;;
esac

NODE="$(command -v node || true)"
if [[ -z "$NODE" ]]; then
  echo "node is not on your PATH. Open a terminal where 'node -v' works and run this again." >&2
  exit 1
fi
NODE="$(readlink -f "$NODE")"

if [[ "${1:-}" == "--print" ]]; then
  for u in service health_service health_timer; do echo "# ---- $u"; "unit_$u"; echo; done
  exit 0
fi

if [[ ! -d "$DIR/node_modules" ]]; then
  echo "Installing dependencies..."
  (cd "$DIR" && npm install --omit=dev)
fi

# A copy started by hand would hold the serial port and the HTTP port.
if ! systemctl is-active --quiet grove-web.service \
   && curl -fsS --max-time 3 -o /dev/null "http://127.0.0.1:$PORT/api/health" 2>/dev/null; then
  echo "Something is already serving on port $PORT, probably a copy started with 'npm start'." >&2
  echo "Stop it (Ctrl+C in its terminal), then run this again." >&2
  exit 1
fi

echo "Installing systemd units (node: $NODE, project: $DIR, user: $RUN_USER)..."
unit_service | sudo tee "$UNIT_DIR/grove-web.service" >/dev/null
unit_health_service | sudo tee "$UNIT_DIR/grove-web-health.service" >/dev/null
unit_health_timer | sudo tee "$UNIT_DIR/grove-web-health.timer" >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable grove-web.service grove-web-health.timer
sudo systemctl restart grove-web.service
sudo systemctl start grove-web-health.timer

printf 'Waiting for the server'
for _ in $(seq 1 20); do
  if curl -fsS --max-time 2 -o /dev/null "http://127.0.0.1:$PORT/api/health" 2>/dev/null; then
    echo " up."
    systemctl --no-pager --lines=0 status grove-web.service | head -3
    echo
    echo "Open http://localhost:$PORT (or this machine's LAN address, port $PORT)."
    echo "It now starts on every boot. Logs: journalctl -u grove-web -f"
    exit 0
  fi
  printf '.'
  sleep 1
done
echo
echo "The server did not come up. Recent logs:" >&2
journalctl -u grove-web --no-pager -n 30 >&2
exit 1
