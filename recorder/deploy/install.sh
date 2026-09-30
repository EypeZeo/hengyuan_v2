#!/usr/bin/env bash
# Install or upgrade the HengYuan D0 recorder bundle on a Debian/Ubuntu host. Run as root.
#
#   install.sh BUNDLE.tar.gz [--enable]
#
# Expects BUNDLE.tar.gz.sha256 next to the bundle, and hy-recorder.service / recorder.toml next to this
# script. Idempotent. Creates only: user hyrec, /opt/hy-recorder, /etc/hy-recorder, /var/lib/hy-recorder,
# /etc/systemd/system/hy-recorder.service. Touches no other service, no firewall, no DNS, no network
# configuration, and does not reboot. The service stays stopped unless --enable is given.
set -euo pipefail

BUNDLE=${1:?usage: install.sh BUNDLE.tar.gz [--enable]}
ENABLE=0
[[ "${2:-}" == "--enable" ]] && ENABLE=1

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/opt/hy-recorder
CONF=/etc/hy-recorder
DATA=/var/lib/hy-recorder
UNIT=/etc/systemd/system/hy-recorder.service
SVC_USER=hyrec

[[ $EUID -eq 0 ]] || { echo "install.sh: run as root" >&2; exit 1; }
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' \
  || { echo "install.sh: python3 >= 3.11 is required" >&2; exit 1; }

BUNDLE=$(readlink -f "$BUNDLE")
( cd "$(dirname "$BUNDLE")" && sha256sum -c "$(basename "$BUNDLE").sha256" )

install -d -m 0755 "$PREFIX" "$PREFIX/releases" "$CONF"
INCOMING=$(mktemp -d "$PREFIX/releases/.incoming.XXXXXX")
trap 'rm -rf "$INCOMING"' EXIT
tar -xzf "$BUNDLE" -C "$INCOMING" --no-same-owner --no-same-permissions
RELEASE=$(ls "$INCOMING")
[[ $(echo "$RELEASE" | wc -w) -eq 1 && "$RELEASE" =~ ^[0-9a-f]{16}$ ]] \
  || { echo "install.sh: unexpected bundle layout: $RELEASE" >&2; exit 1; }
PYTHONPATH="$INCOMING/$RELEASE" python3 -B -m hy_recorder.bundle verify "$INCOMING/$RELEASE" --check-name

if [[ -d "$PREFIX/releases/$RELEASE" ]]; then
  echo "release $RELEASE already installed; re-verifying it"
  PYTHONPATH="$PREFIX/releases/$RELEASE" python3 -B -m hy_recorder.bundle verify "$PREFIX/releases/$RELEASE" --check-name
else
  chown -R root:root "$INCOMING/$RELEASE"
  chmod -R u=rwX,go=rX "$INCOMING/$RELEASE"
  mv "$INCOMING/$RELEASE" "$PREFIX/releases/$RELEASE"
fi

id "$SVC_USER" >/dev/null 2>&1 \
  || useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin "$SVC_USER"
install -d -o "$SVC_USER" -g "$SVC_USER" -m 0750 "$DATA"
[[ -e "$CONF/recorder.toml" ]] || install -m 0644 "$HERE/recorder.toml" "$CONF/recorder.toml"

if ! cmp -s "$HERE/hy-recorder.service" "$UNIT"; then
  install -m 0644 "$HERE/hy-recorder.service" "$UNIT"
  systemctl daemon-reload
fi
systemd-analyze verify "$UNIT" || echo "install.sh: systemd-analyze reported the lines above (review them)" >&2

if [[ -L "$PREFIX/releases/current" ]]; then
  ln -sfn "$(readlink "$PREFIX/releases/current")" "$PREFIX/releases/previous"
fi
ln -sfn "$PREFIX/releases/$RELEASE" "$PREFIX/releases/current.new"
mv -T "$PREFIX/releases/current.new" "$PREFIX/releases/current"

# Smoke test as the service user with the unit's environment (imports, config, derived connections).
runuser -u "$SVC_USER" -- env PYTHONPATH="$PREFIX/releases/current" PYTHONDONTWRITEBYTECODE=1 TZ=UTC \
  python3 -c 'import websockets, zstandard, hy_recorder; print("imports ok:", hy_recorder.__version__)'
runuser -u "$SVC_USER" -- env PYTHONPATH="$PREFIX/releases/current" PYTHONDONTWRITEBYTECODE=1 TZ=UTC \
  python3 -m hy_recorder config-check --config "$CONF/recorder.toml"

echo "installed release $RELEASE (current -> $(readlink "$PREFIX/releases/current"))"
if [[ $ENABLE -eq 1 ]]; then
  systemctl enable --now hy-recorder.service
  echo "hy-recorder enabled and started"
else
  echo "service NOT started. Start with: systemctl enable --now hy-recorder.service"
fi
