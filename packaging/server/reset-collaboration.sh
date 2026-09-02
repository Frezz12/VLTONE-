#!/usr/bin/env bash
# One-time dogfood reset. Accounts, auth, releases and installers are preserved.
set -euo pipefail

ENV_FILE="${VLT_ENV_FILE:-/etc/vlt-account/api.env}"
CONFIRMATION="RESET-COLLABORATION-DOGFOOD"
die() { echo "reset-collaboration: $*" >&2; exit 1; }
[[ $# -eq 6 && "$1" == --database && "$3" == --bucket && "$5" == --confirm ]] ||
    die "usage: $0 --database <name> --bucket <name> --confirm $CONFIRMATION"
expected_database="$2"
expected_bucket="$4"
[[ "$6" == "$CONFIRMATION" ]] || die "confirmation phrase does not match"
for tool in psql aws systemctl; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
[[ -r "$ENV_FILE" ]] || die "missing $ENV_FILE"
systemctl is-active --quiet vlt-account-api && die "vlt-account-api must be stopped"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
[[ "${COLLABORATION_ENABLED:-false}" == false ]] || die "COLLABORATION_ENABLED must be false"
: "${DATABASE_URL:?DATABASE_URL is required}"
: "${COLLAB_OBJECT_BUCKET:?COLLAB_OBJECT_BUCKET is required}"
[[ "$COLLAB_OBJECT_BUCKET" == "$expected_bucket" ]] || die "configured bucket does not match"
actual_database="$(psql "$DATABASE_URL" -XAtc 'SELECT current_database()')"
[[ "$actual_database" == "$expected_database" ]] || die "connected database does not match"

psql "$DATABASE_URL" -X -v ON_ERROR_STOP=1 <<'SQL'
BEGIN;
CREATE TEMP TABLE collaboration_reset_guard AS
SELECT (SELECT count(*) FROM users) AS users,
       (SELECT count(*) FROM web_sessions) AS web_sessions,
       (SELECT count(*) FROM desktop_sessions) AS desktop_sessions,
       (SELECT count(*) FROM releases) AS releases;
DO $$
BEGIN
    IF to_regclass('public.object_cleanup_jobs') IS NOT NULL THEN
        EXECUTE 'DELETE FROM object_cleanup_jobs';
    END IF;
END $$;
DELETE FROM cloud_projects;
DELETE FROM blobs;
DO $$
DECLARE before_counts record;
BEGIN
    SELECT * INTO before_counts FROM collaboration_reset_guard;
    IF before_counts.users <> (SELECT count(*) FROM users)
       OR before_counts.web_sessions <> (SELECT count(*) FROM web_sessions)
       OR before_counts.desktop_sessions <> (SELECT count(*) FROM desktop_sessions)
       OR before_counts.releases <> (SELECT count(*) FROM releases) THEN
        RAISE EXCEPTION 'non-collaboration rows changed; rolling back';
    END IF;
END $$;
COMMIT;
SQL

export AWS_ACCESS_KEY_ID="$COLLAB_OBJECT_ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$COLLAB_OBJECT_SECRET_ACCESS_KEY"
export AWS_DEFAULT_REGION="${COLLAB_OBJECT_REGION:-us-east-1}"
if [[ -n "${COLLAB_OBJECT_SESSION_TOKEN:-}" ]]; then export AWS_SESSION_TOKEN="$COLLAB_OBJECT_SESSION_TOKEN"; fi
endpoint=()
if [[ -n "${COLLAB_OBJECT_ENDPOINT:-}" ]]; then endpoint=(--endpoint-url "$COLLAB_OBJECT_ENDPOINT"); fi

while IFS=$'\t' read -r key upload_id; do
    [[ -n "$key" && "$key" != None && -n "$upload_id" ]] || continue
    aws "${endpoint[@]}" s3api abort-multipart-upload --bucket "$expected_bucket" \
        --key "$key" --upload-id "$upload_id"
done < <(aws "${endpoint[@]}" s3api list-multipart-uploads \
             --bucket "$expected_bucket" --prefix uploads/ \
             --query 'Uploads[].[Key,UploadId]' --output text)
aws "${endpoint[@]}" s3 rm "s3://$expected_bucket/uploads/" --recursive --only-show-errors
aws "${endpoint[@]}" s3 rm "s3://$expected_bucket/blobs/" --recursive --only-show-errors
echo "Collaboration dogfood data reset; account/auth/release rows were preserved."
