#!/usr/bin/env bash
# Verify and install an immutable bundle, then deploy it unless --no-deploy is set.
set -euo pipefail

APP_ROOT="${VLT_APP_ROOT:-/opt/vlt-account-platform}"
SYSTEM_USER="${VLT_SYSTEM_USER:-vltaccount}"
SYSTEM_GROUP="${VLT_SYSTEM_GROUP:-vltaccount}"
ENV_DIR="${VLT_ENV_DIR:-/etc/vlt-account}"

die() { echo "install: $*" >&2; exit 1; }
[[ ${EUID} -eq 0 ]] || die "run as root (or through sudo)"
[[ $# -ge 1 && $# -le 2 ]] || die "usage: $0 <bundle.tar.gz> [--no-deploy]"
[[ ${2:-} == "" || ${2:-} == "--no-deploy" ]] || die "unknown option: $2"
for tool in tar sha256sum systemctl systemd-tmpfiles readlink; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
id "$SYSTEM_USER" >/dev/null 2>&1 || die "missing system user: $SYSTEM_USER"
getent group "$SYSTEM_GROUP" >/dev/null || die "missing system group: $SYSTEM_GROUP"

archive="$(readlink -e -- "$1")" || die "bundle does not exist: $1"
checksum="$archive.sha256"
[[ -r "$checksum" ]] || die "missing checksum: $checksum"
(cd "$(dirname "$archive")" && sha256sum -c "$(basename "$checksum")")

mapfile -t roots < <(tar -tzf "$archive" | awk -F/ 'NF {print $1}' | sort -u)
[[ ${#roots[@]} -eq 1 ]] || die "bundle must contain one top-level directory"
root_name="${roots[0]}"
version="${root_name#vlt-account-platform-}"
case "$root_name:$version" in
    vlt-account-platform-*:*[!A-Za-z0-9._+-]*|vlt-account-platform-:) die "invalid bundle root: $root_name" ;;
    vlt-account-platform-*:*) ;;
    *) die "invalid bundle root: $root_name" ;;
esac
while IFS= read -r entry; do
    case "/$entry/" in
        //*|*/../*|*/./*) die "unsafe archive entry: $entry" ;;
    esac
done < <(tar -tzf "$archive")

install -d -m 0755 "$APP_ROOT/releases"
release="$APP_ROOT/releases/$version"
[[ ! -e "$release" ]] || die "release already exists: $release"
incoming="$(mktemp -d "$APP_ROOT/releases/.incoming.XXXXXX")"
cleanup() {
    case "$incoming" in "$APP_ROOT/releases/.incoming."*) rm -rf -- "$incoming" ;; esac
}
trap cleanup EXIT
tar -xzf "$archive" -C "$incoming" --no-same-owner
[[ ! -L "$incoming/$root_name" ]] || die "bundle root must not be a symlink"
find "$incoming/$root_name" -type l -print -quit | grep -q . && die "bundle contains symlinks"
(cd "$incoming/$root_name" && sha256sum -c SHA256SUMS)
chown -R root:root "$incoming/$root_name"
mv -- "$incoming/$root_name" "$release"

install -d -m 0750 -o root -g "$SYSTEM_GROUP" "$ENV_DIR"
install -m 0644 "$release/ops/api.env.example" "$ENV_DIR/api.env.example"
install -m 0644 "$release/ops/apache/vlt-account.conf" "$ENV_DIR/apache-vhost.example.conf"
install -m 0644 "$release/ops/systemd/"*.service /etc/systemd/system/
install -m 0644 "$release/ops/tmpfiles.d/vlt-account.conf" /etc/tmpfiles.d/
systemd-tmpfiles --create /etc/tmpfiles.d/vlt-account.conf
systemctl daemon-reload
systemctl enable vlt-account-api vlt-account-web vlt-account-admin

echo "Installed $release"
if [[ ${2:-} == "--no-deploy" ]]; then
    exit 0
fi
[[ -r "$ENV_DIR/api.env" ]] || die "create $ENV_DIR/api.env from api.env.example, then run deploy.sh"
exec "$release/ops/deploy.sh" "$release"
