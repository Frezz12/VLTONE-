#!/usr/bin/env bash
# Apply forward migrations and atomically activate one installed release.
set -euo pipefail

APP_ROOT="${VLT_APP_ROOT:-/opt/vlt-account-platform}"
SYSTEM_USER="${VLT_SYSTEM_USER:-vltaccount}"
ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
SERVICES=(vlt-account-api vlt-account-web vlt-account-admin)

die() { echo "deploy: $*" >&2; exit 1; }
[[ ${EUID} -eq 0 ]] || die "run as root (or through sudo)"
[[ $# -eq 1 ]] || die "usage: $0 /opt/vlt-account-platform/releases/<version>"

release="$(readlink -e -- "$1")" || die "release does not exist: $1"
releases_root="$(readlink -m -- "$APP_ROOT/releases")"
[[ "$(dirname -- "$release")" == "$releases_root" ]] ||
    die "release must be directly below $releases_root"
[[ -x "$release/bin/vlt-migrate" ]] || die "vlt-migrate is missing"
[[ -x "$release/ops/preflight.sh" ]] || die "preflight is missing"
[[ -r "$ENV_FILE" ]] || die "missing $ENV_FILE"

"$release/ops/preflight.sh" "$release"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
systemctl stop "${SERVICES[@]}"
if ! runuser -u "$SYSTEM_USER" -- "$release/bin/vlt-migrate" up; then
    # Expand-only migrations keep the previous application compatible. If a
    # migration itself fails, restore availability without changing current.
    systemctl start "${SERVICES[@]}" || true
    die "database migration failed; current release was not changed"
fi

replace_link() {
    local name="$1"
    local target="$2"
    local temporary="$APP_ROOT/.${name}.$$"
    rm -f -- "$temporary"
    ln -s -- "$target" "$temporary"
    mv -Tf -- "$temporary" "$APP_ROOT/$name"
}

previous=""
if [[ -L "$APP_ROOT/current" ]]; then
    previous="$(readlink -e -- "$APP_ROOT/current")" || true
fi
if [[ -n "$previous" ]]; then
    [[ "$(dirname -- "$previous")" == "$releases_root" ]] ||
        die "current points outside $releases_root"
    replace_link previous "$previous"
fi
replace_link current "$release"

install -m 0644 "$release/ops/systemd/"*.service /etc/systemd/system/
systemctl daemon-reload
systemctl start "${SERVICES[@]}"
if "$release/ops/smoke.sh"; then
    systemctl --no-pager --full status "${SERVICES[@]}"
    echo "Activated $release"
    exit 0
fi

if [[ -n "$previous" ]]; then
    echo "Smoke test failed; restoring $previous" >&2
    replace_link current "$previous"
    replace_link previous "$release"
    install -m 0644 "$previous/ops/systemd/"*.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl restart "${SERVICES[@]}"
    if ! "$previous/ops/smoke.sh"; then
        echo "Previous release was restored but its smoke test also failed." >&2
    fi
else
    echo "Smoke test failed and no previous release is installed." >&2
fi
exit 1
