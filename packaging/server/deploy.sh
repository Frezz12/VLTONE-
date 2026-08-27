#!/usr/bin/env bash
# Rebuild and restart the VLT account platform on a prepared Ubuntu server.
# Provision PostgreSQL, Node.js 24, Go 1.26.7, Apache and the systemd unit
# files once (see README.md); this script is safe to run for each update.
set -euo pipefail

APP_ROOT="${VLT_APP_ROOT:-/opt/vlt-account-platform}"
SYSTEM_USER="${VLT_SYSTEM_USER:-vltaccount}"
ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"

if [[ ${EUID} -ne 0 ]]; then
    echo "Run as root (or through sudo)." >&2
    exit 1
fi
for tool in go node corepack; do
    command -v "$tool" >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
[[ -r "$ENV_FILE" ]] || { echo "Missing $ENV_FILE." >&2; exit 1; }
[[ -d "$APP_ROOT/backend" && -f "$APP_ROOT/pnpm-lock.yaml" ]] || {
    echo "Not a VLT platform checkout: $APP_ROOT" >&2; exit 1;
}

install -d -o "$SYSTEM_USER" -g "$SYSTEM_USER" "$APP_ROOT/bin"
runuser -u "$SYSTEM_USER" -- bash -c "
  set -euo pipefail
  export PATH=/usr/local/bin:\$PATH
  export GOCACHE=/var/lib/vlt-account/.cache/go-build
  cd '$APP_ROOT/backend'
  go build -trimpath -ldflags='-s -w' -o '$APP_ROOT/bin/vlt-api' ./cmd/api
  go build -trimpath -ldflags='-s -w' -o '$APP_ROOT/bin/vlt-migrate' ./cmd/migrate
  go build -trimpath -ldflags='-s -w' -o '$APP_ROOT/bin/vlt-adminctl' ./cmd/adminctl
"

corepack enable
runuser -u "$SYSTEM_USER" -- bash -c "
  set -euo pipefail
  export COREPACK_HOME=/var/lib/vlt-account/.cache/corepack
  cd '$APP_ROOT'
  pnpm install --frozen-lockfile
  pnpm generate:api
  NODE_OPTIONS=--max-old-space-size=1024 pnpm --filter @vlt/api-client build
  NODE_OPTIONS=--max-old-space-size=1024 pnpm --filter @vlt/web build
  NODE_OPTIONS=--max-old-space-size=1024 pnpm --filter @vlt/admin build
"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
runuser -u "$SYSTEM_USER" -- "$APP_ROOT/bin/vlt-migrate" up
systemctl daemon-reload
systemctl enable --now vlt-account-api vlt-account-web vlt-account-admin
systemctl restart vlt-account-api vlt-account-web vlt-account-admin
systemctl --no-pager --full status vlt-account-api vlt-account-web vlt-account-admin
