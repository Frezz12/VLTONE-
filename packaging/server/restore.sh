#!/usr/bin/env bash
# Guarded disaster restore. Never used for ordinary application rollback.
set -euo pipefail

ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
CONFIRMATION="RESTORE-VLT-ACCOUNT-PRODUCTION"
die() { echo "restore: $*" >&2; exit 1; }
[[ $# -eq 7 ]] || die "usage: $0 <backup-directory> --database <name> --bucket <name> --confirm $CONFIRMATION"
backup="$1"
[[ "$2" == --database && "$4" == --bucket && "$6" == --confirm ]] || die "invalid arguments"
expected_database="$3"
expected_bucket="$5"
[[ "$7" == "$CONFIRMATION" ]] || die "confirmation phrase does not match"
for tool in pg_restore psql aws sha256sum systemctl awk; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ -d "$backup" && -r "$backup/MANIFEST" && -r "$backup/SHA256SUMS" &&
   -r "$backup/postgresql.dump" ]] || die "backup is incomplete"
for unit in vlt-account-api vlt-account-web vlt-account-admin; do
    systemctl is-active --quiet "$unit" && die "$unit must be stopped"
done

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
: "${DATABASE_URL:?DATABASE_URL is required}"
: "${COLLAB_OBJECT_BUCKET:?COLLAB_OBJECT_BUCKET is required}"
manifest_value() { awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$backup/MANIFEST"; }
[[ "$(manifest_value format)" == vlt-production-backup-v1 ]] || die "unsupported backup format"
[[ "$(manifest_value database)" == "$expected_database" ]] || die "manifest database does not match"
[[ "$(manifest_value bucket)" == "$expected_bucket" ]] || die "manifest bucket does not match"
[[ "$COLLAB_OBJECT_BUCKET" == "$expected_bucket" ]] || die "configured bucket does not match"
actual_database="$(psql "$DATABASE_URL" -XAtc 'SELECT current_database()')"
[[ "$actual_database" == "$expected_database" ]] || die "connected database does not match"
(cd "$backup" && sha256sum -c SHA256SUMS)

export AWS_ACCESS_KEY_ID="$COLLAB_OBJECT_ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$COLLAB_OBJECT_SECRET_ACCESS_KEY"
export AWS_DEFAULT_REGION="${COLLAB_OBJECT_REGION:-us-east-1}"
if [[ -n "${COLLAB_OBJECT_SESSION_TOKEN:-}" ]]; then export AWS_SESSION_TOKEN="$COLLAB_OBJECT_SESSION_TOKEN"; fi
endpoint=()
if [[ -n "${COLLAB_OBJECT_ENDPOINT:-}" ]]; then endpoint=(--endpoint-url "$COLLAB_OBJECT_ENDPOINT"); fi

# Restore the relational source of truth transactionally, then replace only
# the two collaboration-owned S3 prefixes. A fresh backup is an operator gate,
# not something this destructive command silently creates in the same target.
pg_restore --dbname="$DATABASE_URL" --clean --if-exists --no-owner \
    --no-privileges --single-transaction "$backup/postgresql.dump"
aws "${endpoint[@]}" s3 rm "s3://$expected_bucket/uploads/" --recursive --only-show-errors
aws "${endpoint[@]}" s3 rm "s3://$expected_bucket/blobs/" --recursive --only-show-errors
aws "${endpoint[@]}" s3 sync "$backup/s3/uploads/" "s3://$expected_bucket/uploads/" --only-show-errors
aws "${endpoint[@]}" s3 sync "$backup/s3/blobs/" "s3://$expected_bucket/blobs/" --only-show-errors
echo "Restore completed. Run migrations and smoke tests before starting services."
