#!/usr/bin/env bash
# Build the immutable Linux server artifact. Run this in CI, not on production.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always --dirty)}"
OUTPUT="${2:-$ROOT/dist/server}"
BUNDLE="vlt-account-platform-$VERSION"

case "$VERSION" in
    *[!A-Za-z0-9._+-]*|'') echo "invalid release version: $VERSION" >&2; exit 1 ;;
esac
for tool in go node corepack git tar sha256sum find; do
    command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done
[[ "$(uname -s)" == Linux ]] || { echo "server bundles must be built on Linux" >&2; exit 1; }
[[ "$(node -p 'process.versions.node.split(`.`)[0]')" == 24 ]] || {
    echo "Node.js 24 is required" >&2; exit 1;
}
[[ "$(corepack pnpm --version)" == 11.3.0 ]] || {
    echo "pnpm 11.3.0 is required" >&2; exit 1;
}
[[ "$(cd "$ROOT/backend" && go version)" == *" go1.26.7 "* ]] || {
    echo "Go toolchain 1.26.7 is required" >&2; exit 1;
}
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=normal)" ]] || {
    echo "refusing to build a release from a dirty worktree" >&2; exit 1;
}

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
stage="$work/$BUNDLE"
mkdir -p "$stage/bin" "$stage/migrations" "$stage/ops" "$OUTPUT"

cd "$ROOT"
corepack pnpm install --frozen-lockfile
corepack pnpm generate:api
git diff --exit-code -- packages/api-client/src/schema.d.ts

(cd backend &&
    go build -trimpath -ldflags='-s -w' -o "$stage/bin/vlt-api" ./cmd/api &&
    go build -trimpath -ldflags='-s -w' -o "$stage/bin/vlt-migrate" ./cmd/migrate &&
    go build -trimpath -ldflags='-s -w' -o "$stage/bin/vlt-adminctl" ./cmd/adminctl)

VLT_API_ORIGIN=http://127.0.0.1:8080 \
    NODE_OPTIONS=--max-old-space-size=1024 corepack pnpm build

copy_next_app() {
    local app="$1" destination="$stage/$1" standalone="$ROOT/$1/.next/standalone"
    [[ -d "$standalone" ]] || { echo "$app standalone output is missing" >&2; exit 1; }
    mkdir -p "$destination"
    cp -aL "$standalone/." "$destination/"

    local entry=""
    if [[ -f "$destination/server.js" ]]; then
        entry="server.js"
    elif [[ -f "$destination/$app/server.js" ]]; then
        entry="$app/server.js"
    else
        mapfile -t entries < <(find "$destination" -type f -path "*/$app/server.js" -printf '%P\n')
        [[ ${#entries[@]} -eq 1 ]] || {
            echo "could not identify the $app standalone server.js" >&2; exit 1;
        }
        entry="${entries[0]}"
    fi

    local app_root="$destination/$(dirname "$entry")"
    if [[ -d "$ROOT/$app/public" ]]; then
        mkdir -p "$app_root/public"
        cp -aL "$ROOT/$app/public/." "$app_root/public/"
    fi
    mkdir -p "$app_root/.next"
    mkdir -p "$app_root/.next/static"
    cp -aL "$ROOT/$app/.next/static/." "$app_root/.next/static/"
    cat > "$destination/start" <<EOF
#!/usr/bin/env sh
set -eu
root=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
NODE_PATH="\$root/node_modules/.pnpm/node_modules\${NODE_PATH:+:\$NODE_PATH}"
export NODE_PATH
exec /usr/bin/node "\$root/$entry"
EOF
    chmod 0755 "$destination/start"

    # Dereferencing the standalone pnpm symlinks keeps the release archive
    # self-contained, but transitive packages still live in the virtual
    # store. Exercise Next's server dependency graph before packaging so a
    # missing NODE_PATH can never reach deployment.
    (cd "$app_root" &&
        NODE_PATH="$destination/node_modules/.pnpm/node_modules" \
            node -e "require('next/dist/server/next')")
}

copy_next_app web
copy_next_app admin
cp backend/migrations/*.sql "$stage/migrations/"
cp -aL packaging/server/. "$stage/ops/"
chmod 0755 "$stage/ops/"*.sh
printf '%s\n' "$VERSION" > "$stage/VERSION"
git rev-parse HEAD > "$stage/BUILD_COMMIT"

(cd "$stage" &&
    find . -type f ! -name SHA256SUMS -print0 |
        sort -z | xargs -0 sha256sum > SHA256SUMS)

archive="$OUTPUT/$BUNDLE.tar.gz"
[[ ! -e "$archive" && ! -e "$archive.sha256" ]] || {
    echo "release artifact already exists: $archive" >&2; exit 1;
}
tar -C "$work" -czf "$archive" "$BUNDLE"
(cd "$OUTPUT" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
echo "$archive"
