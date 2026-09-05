#!/usr/bin/env bash
# Validate a release and its production dependencies without changing them.
set -euo pipefail

APP_ROOT="${VLT_APP_ROOT:-/opt/vlt-account-platform}"
SYSTEM_USER="${VLT_SYSTEM_USER:-vltaccount}"
SYSTEM_GROUP="${VLT_SYSTEM_GROUP:-vltaccount}"
ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
release="${1:-$APP_ROOT/current}"

die() { echo "preflight: $*" >&2; exit 1; }
for tool in curl node pg_isready sha256sum systemctl systemd-analyze apachectl runuser stat df readlink; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ -x /usr/bin/node && "$(/usr/bin/node -p 'process.versions.node.split(`.`)[0]')" == 24 ]] ||
    die "Node.js 24 must be installed at /usr/bin/node"
release="$(readlink -e -- "$release")" || die "release does not exist"
[[ -x "$release/bin/vlt-api" && -x "$release/bin/vlt-migrate" ]] || die "release binaries are incomplete"
[[ -x "$release/web/start" && -x "$release/admin/start" ]] || die "standalone web bundles are incomplete"
(cd "$release" && sha256sum -c SHA256SUMS >/dev/null)

[[ -r "$ENV_FILE" ]] || die "missing $ENV_FILE"
owner="$(stat -c '%U' "$ENV_FILE")"
mode="$(stat -c '%a' "$ENV_FILE")"
[[ "$owner" == root ]] || die "$ENV_FILE must be owned by root"
[[ "$mode" == 600 ]] || die "$ENV_FILE must have mode 0600"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
require() { [[ -n "${!1:-}" ]] || die "$1 is required"; }
positive() { [[ "${!1:-}" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"; }
for name in DATABASE_URL PUBLIC_ORIGIN ADMIN_ORIGIN DESKTOP_API_ORIGIN \
            AUTH_SIGNING_SEED STORAGE_ROOT AI_GLOBAL_MONTHLY_TOKEN_LIMIT \
            SMTP_HOST SMTP_FROM; do
    require "$name"
    [[ "${!name}" != *CHANGE_ME* ]] || die "$name still contains CHANGE_ME"
done
if [[ -n "${SMTP_USERNAME:-}" && -z "${SMTP_PASSWORD:-}" ]] ||
   [[ -z "${SMTP_USERNAME:-}" && -n "${SMTP_PASSWORD:-}" ]]; then
    die "SMTP_USERNAME and SMTP_PASSWORD must be set together"
fi
[[ "${APP_ENV:-}" == production ]] || die "APP_ENV must be production"
[[ "${HTTP_ADDR:-}" == 127.0.0.1:8080 ]] || die "HTTP_ADDR must be 127.0.0.1:8080"
[[ "$PUBLIC_ORIGIN" == https://* && "$ADMIN_ORIGIN" == https://* &&
   "$DESKTOP_API_ORIGIN" == https://* ]] || die "all public origins must use HTTPS"
positive AI_GLOBAL_MONTHLY_TOKEN_LIMIT
if [[ "${AI_ENABLED:-true}" == true ]]; then
    require AI_CREDENTIALS_KEY
    [[ "$AI_CREDENTIALS_KEY" != *CHANGE_ME* ]] || die "AI_CREDENTIALS_KEY still contains CHANGE_ME"
fi
pg_isready -d "$DATABASE_URL" >/dev/null || die "PostgreSQL is not ready"
[[ -d "$STORAGE_ROOT" ]] || die "STORAGE_ROOT does not exist: $STORAGE_ROOT"
runuser -u "$SYSTEM_USER" -- test -w "$STORAGE_ROOT" || die "$STORAGE_ROOT is not writable by $SYSTEM_USER"

available_kib="$(df -Pk "$APP_ROOT" | awk 'NR==2 {print $4}')"
[[ "$available_kib" =~ ^[0-9]+$ && "$available_kib" -ge 5242880 ]] ||
    die "at least 5 GiB free space is required"

if [[ "${COLLABORATION_ENABLED:-false}" == true ]]; then
    require VLT_INVITE_CODE_PEPPER
    [[ "$VLT_INVITE_CODE_PEPPER" != *CHANGE_ME* ]] ||
        die "VLT_INVITE_CODE_PEPPER still contains CHANGE_ME"
    [[ "${COLLAB_RECORDING_ENABLED:-false}" == false ]] || die "cloud recording must remain disabled for V1"
    # Per-account entitlement is managed in the admin UI and defaults to
    # false in PostgreSQL. This optional env list remains an emergency OR
    # override, so an empty value is still safely default-deny.
    for name in COLLAB_OBJECT_ENDPOINT COLLAB_OBJECT_REGION COLLAB_OBJECT_BUCKET \
                COLLAB_OBJECT_ACCESS_KEY_ID COLLAB_OBJECT_SECRET_ACCESS_KEY; do
        require "$name"
        [[ "${!name}" != *CHANGE_ME* ]] || die "$name still contains CHANGE_ME"
    done
    [[ "$COLLAB_OBJECT_ENDPOINT" == https://* ]] || die "COLLAB_OBJECT_ENDPOINT must use HTTPS"
    for name in COLLAB_MAX_OBJECT_BYTES COLLAB_PROJECT_QUOTA_BYTES COLLAB_USER_QUOTA_BYTES \
                COLLAB_MAX_OPEN_UPLOADS_PER_USER COLLAB_MAX_OPEN_UPLOADS_PER_PROJECT \
                COLLAB_VERIFY_WORKERS COLLAB_MAX_VERIFY_PER_USER; do
        positive "$name"
    done
    status="$(curl --silent --show-error --output /dev/null --write-out '%{http_code}' \
        --connect-timeout 5 --max-time 10 "$COLLAB_OBJECT_ENDPOINT")" ||
        die "object storage endpoint is unreachable"
    [[ "$status" != 000 ]] || die "object storage endpoint is unreachable"
fi

if [[ -e "$APP_ROOT/current" ]]; then
    systemd-analyze verify "$release/ops/systemd/"*.service >/dev/null
fi
apachectl configtest >/dev/null
echo "Preflight passed for $release"
