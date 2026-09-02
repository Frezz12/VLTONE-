#!/usr/bin/env bash
# Switch back to the previously installed application. Never down-migrate.
set -euo pipefail

APP_ROOT="${VLT_APP_ROOT:-/opt/vlt-account-platform}"
SERVICES=(vlt-account-api vlt-account-web vlt-account-admin)
die() { echo "rollback: $*" >&2; exit 1; }
[[ ${EUID} -eq 0 ]] || die "run as root (or through sudo)"
[[ $# -le 1 ]] || die "usage: $0 [installed-release]"

current="$(readlink -e -- "$APP_ROOT/current")" || die "current release is missing"
target="$(readlink -e -- "${1:-$APP_ROOT/previous}")" || die "previous release is missing"
releases_root="$(readlink -m -- "$APP_ROOT/releases")"
for release in "$current" "$target"; do
    [[ "$(dirname -- "$release")" == "$releases_root" ]] ||
        die "$release is outside $releases_root"
done
[[ "$current" != "$target" ]] || die "target is already current"
"$current/ops/preflight.sh" "$target"

replace_link() {
    local name="$1"
    local value="$2"
    local temporary="$APP_ROOT/.${name}.$$"
    rm -f -- "$temporary"
    ln -s -- "$value" "$temporary"
    mv -Tf -- "$temporary" "$APP_ROOT/$name"
}
replace_link current "$target"
replace_link previous "$current"
install -m 0644 "$target/ops/systemd/"*.service /etc/systemd/system/
systemctl daemon-reload
systemctl restart "${SERVICES[@]}"
if "$current/ops/smoke.sh"; then
    echo "Rolled back application to $target (database migrations were retained)."
    exit 0
fi

echo "Rollback smoke test failed; restoring $current" >&2
replace_link current "$current"
replace_link previous "$target"
install -m 0644 "$current/ops/systemd/"*.service /etc/systemd/system/
systemctl daemon-reload
systemctl restart "${SERVICES[@]}"
if ! "$current/ops/smoke.sh"; then
    echo "Original release was restored but its smoke test also failed." >&2
fi
exit 1
