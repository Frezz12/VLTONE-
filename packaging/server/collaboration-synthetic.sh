#!/usr/bin/env bash
# End-to-end S3 collaboration probe. Run from monitoring independently of
# /readyz so an object-store outage never removes the account API from service.
set -euo pipefail

ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
[[ -r "$ENV_FILE" ]] || { echo "collaboration synthetic: missing $ENV_FILE" >&2; exit 1; }
command -v aws >/dev/null || { echo "collaboration synthetic: aws CLI is required" >&2; exit 1; }
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

: "${COLLAB_OBJECT_BUCKET:?COLLAB_OBJECT_BUCKET is required}"
: "${COLLAB_OBJECT_ACCESS_KEY_ID:?COLLAB_OBJECT_ACCESS_KEY_ID is required}"
: "${COLLAB_OBJECT_SECRET_ACCESS_KEY:?COLLAB_OBJECT_SECRET_ACCESS_KEY is required}"
export AWS_ACCESS_KEY_ID="$COLLAB_OBJECT_ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$COLLAB_OBJECT_SECRET_ACCESS_KEY"
export AWS_DEFAULT_REGION="${COLLAB_OBJECT_REGION:-us-east-1}"
if [[ -n "${COLLAB_OBJECT_SESSION_TOKEN:-}" ]]; then
    export AWS_SESSION_TOKEN="$COLLAB_OBJECT_SESSION_TOKEN"
fi
endpoint=()
if [[ -n "${COLLAB_OBJECT_ENDPOINT:-}" ]]; then
    endpoint=(--endpoint-url "$COLLAB_OBJECT_ENDPOINT")
fi

probe_id="$(tr -d '\r\n' </proc/sys/kernel/random/uuid)"
[[ "$probe_id" =~ ^[0-9a-f-]{36}$ ]] || {
    echo "collaboration synthetic: could not create probe identity" >&2
    exit 1
}
object_key="uploads/synthetic/$probe_id"
work="$(mktemp -d)"
cleanup() {
    aws "${endpoint[@]}" s3api delete-object --bucket "$COLLAB_OBJECT_BUCKET" \
        --key "$object_key" >/dev/null 2>&1 || true
    rm -rf -- "$work"
}
trap cleanup EXIT

printf 'vlt-collaboration-synthetic:%s\n' "$probe_id" > "$work/source"
expected_size="$(stat -c '%s' "$work/source")"
expected_sha="$(sha256sum "$work/source" | awk '{print $1}')"

aws "${endpoint[@]}" s3api put-object --bucket "$COLLAB_OBJECT_BUCKET" \
    --key "$object_key" --body "$work/source" \
    --content-type application/octet-stream >/dev/null
observed_size="$(aws "${endpoint[@]}" s3api head-object \
    --bucket "$COLLAB_OBJECT_BUCKET" --key "$object_key" \
    --query ContentLength --output text)"
[[ "$observed_size" == "$expected_size" ]] || {
    echo "collaboration synthetic: object size mismatch" >&2
    exit 1
}
aws "${endpoint[@]}" s3api get-object --bucket "$COLLAB_OBJECT_BUCKET" \
    --key "$object_key" "$work/download" >/dev/null
observed_sha="$(sha256sum "$work/download" | awk '{print $1}')"
[[ "$observed_sha" == "$expected_sha" ]] || {
    echo "collaboration synthetic: object checksum mismatch" >&2
    exit 1
}
aws "${endpoint[@]}" s3api get-bucket-lifecycle-configuration \
    --bucket "$COLLAB_OBJECT_BUCKET" >/dev/null
echo "Collaboration object-storage synthetic passed"
