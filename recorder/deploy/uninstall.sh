#!/usr/bin/env bash
# Roll back everything install.sh created. Run as root. The data directory is kept unless --purge-data.
#
#   uninstall.sh [--purge-data]
#
# Removes: the hy-recorder service and unit, /opt/hy-recorder, /etc/hy-recorder, user hyrec.
# Touches nothing else on the host.
set -euo pipefail

PURGE=0
[[ "${1:-}" == "--purge-data" ]] && PURGE=1
[[ $EUID -eq 0 ]] || { echo "uninstall.sh: run as root" >&2; exit 1; }

if systemctl list-unit-files hy-recorder.service >/dev/null 2>&1; then
  systemctl disable --now hy-recorder.service 2>/dev/null || true
fi
rm -f /etc/systemd/system/hy-recorder.service
systemctl daemon-reload
rm -rf /opt/hy-recorder /etc/hy-recorder
if [[ $PURGE -eq 1 ]]; then
  rm -rf /var/lib/hy-recorder
  echo "data directory removed"
else
  echo "data directory kept: /var/lib/hy-recorder"
fi
if id hyrec >/dev/null 2>&1; then
  userdel hyrec 2>/dev/null || echo "uninstall.sh: could not remove user hyrec (files still owned by it?)" >&2
fi
echo "hy-recorder removed"
