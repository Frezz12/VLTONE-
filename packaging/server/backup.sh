#!/usr/bin/env bash
# Consistent production backup of PostgreSQL and collaboration/forum object prefixes.
set -euo pipefail

ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
die() { echo "backup: $*" >&2; exit 1; }
[[ $# -eq 1 ]] || die "usage: $0 <new-backup-directory>"
for tool in pg_dump psql aws sha256sum find sort xargs date systemctl; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ -r "$ENV_FILE" ]] || die "missing $ENV_FILE"
[[ ! -e "$1" ]] || die "destination already exists: $1"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
: "${DATABASE_URL:?DATABASE_URL is required}"
: "${COLLAB_OBJECT_BUCKET:?COLLAB_OBJECT_BUCKET is required}"
: "${COLLAB_OBJECT_ACCESS_KEY_ID:?COLLAB_OBJECT_ACCESS_KEY_ID is required}"
: "${COLLAB_OBJECT_SECRET_ACCESS_KEY:?COLLAB_OBJECT_SECRET_ACCESS_KEY is required}"

# Immutable files are removed by cleanup workers. Pause writers/cleanup for
# the database snapshot and object copy to represent one recoverable state.
for unit in vlt-account-api vlt-forum-media; do
    systemctl is-active --quiet "$unit" && die "stop $unit before a consistent backup"
done
: "${FORUM_OBJECT_BUCKET:?FORUM_OBJECT_BUCKET is required}"
: "${FORUM_OBJECT_ENDPOINT:?FORUM_OBJECT_ENDPOINT is required}"
: "${FORUM_OBJECT_ACCESS_KEY_ID:?FORUM_OBJECT_ACCESS_KEY_ID is required}"
: "${FORUM_OBJECT_SECRET_ACCESS_KEY:?FORUM_OBJECT_SECRET_ACCESS_KEY is required}"
destination="$1"
install -d -m 0700 "$destination" "$destination/s3/uploads" "$destination/s3/blobs"
database_name="$(psql "$DATABASE_URL" -XAtc 'SELECT current_database()')"
[[ -n "$database_name" ]] || die "could not identify the database"

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

pg_dump "$DATABASE_URL" --format=custom --compress=6 --no-owner \
    --file="$destination/postgresql.dump"
aws "${endpoint[@]}" s3 sync "s3://$COLLAB_OBJECT_BUCKET/uploads/" \
    "$destination/s3/uploads/" --only-show-errors
aws "${endpoint[@]}" s3 sync "s3://$COLLAB_OBJECT_BUCKET/blobs/" \
    "$destination/s3/blobs/" --only-show-errors

export AWS_ACCESS_KEY_ID="$FORUM_OBJECT_ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$FORUM_OBJECT_SECRET_ACCESS_KEY"
export AWS_DEFAULT_REGION="${FORUM_OBJECT_REGION:-us-east-1}"
export AWS_SESSION_TOKEN="${FORUM_OBJECT_SESSION_TOKEN:-}"
for prefix in objects media staging; do
    install -d -m 0700 "$destination/forum/$prefix"
    aws --endpoint-url "$FORUM_OBJECT_ENDPOINT" s3 sync "s3://$FORUM_OBJECT_BUCKET/$prefix/" "$destination/forum/$prefix/" --only-show-errors
done
cat >"$destination/MANIFEST" <<EOF
format=vlt-production-backup-v1
created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
database=$database_name
bucket=$COLLAB_OBJECT_BUCKET
forum_bucket=$FORUM_OBJECT_BUCKET
EOF
(cd "$destination" &&
    find . -type f ! -name SHA256SUMS -print0 |
        sort -z | xargs -0 sha256sum >SHA256SUMS)
chmod -R go-rwx "$destination"
echo "Backup completed: $destination"
