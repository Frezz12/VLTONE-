#!/usr/bin/env bash
# Verify the three local services and the externally routed HTTPS endpoints.
set -euo pipefail

ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
SERVICES=(vlt-account-api vlt-account-web vlt-account-admin)
[[ -r "$ENV_FILE" ]] || { echo "smoke: missing $ENV_FILE" >&2; exit 1; }
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

for service in "${SERVICES[@]}"; do
    systemctl is-active --quiet "$service" || { echo "smoke: $service is not active" >&2; exit 1; }
done

retry_get() {
    local url="$1" output="$2"
    for _ in $(seq 1 30); do
        if curl --fail --silent --show-error --max-time 5 "$url" > "$output"; then return 0; fi
        sleep 1
    done
    echo "smoke: $url did not become ready" >&2
    return 1
}

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
retry_get http://127.0.0.1:8080/healthz "$tmp/health.json"
retry_get http://127.0.0.1:8080/readyz "$tmp/ready.json"
retry_get http://127.0.0.1:8080/v1/meta "$tmp/meta.json"
grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"' "$tmp/health.json"
grep -Eq '"status"[[:space:]]*:[[:space:]]*"ready"' "$tmp/ready.json"
grep -Eq '"protocol"[[:space:]]*:[[:space:]]*"vlt-collab-v2"' "$tmp/meta.json"
grep -Eq '"project_format"[[:space:]]*:[[:space:]]*7' "$tmp/meta.json"
grep -Eq '"command_schema"[[:space:]]*:[[:space:]]*2' "$tmp/meta.json"
grep -Eq '"recording"[[:space:]]*:[[:space:]]*false' "$tmp/meta.json"

if [[ "${COLLABORATION_ENABLED:-false}" == true ]]; then
    grep -Eq '"enabled"[[:space:]]*:[[:space:]]*true' "$tmp/meta.json"
else
    grep -Eq '"enabled"[[:space:]]*:[[:space:]]*false' "$tmp/meta.json"
fi

retry_get http://127.0.0.1:3100/ru "$tmp/web.html"
retry_get http://127.0.0.1:3101/login "$tmp/admin.html"
retry_get "${PUBLIC_ORIGIN%/}/api/v1/meta" "$tmp/public-meta.json"
retry_get "${ADMIN_ORIGIN%/}/login" "$tmp/public-admin.html"
echo "Smoke tests passed"
