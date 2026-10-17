# VLT account platform — immutable Ubuntu deployment

Production runs one API process and two standalone Next.js processes behind
Apache. Releases are prebuilt in CI and installed under
`/opt/vlt-account-platform/releases/<version>`; `current` and `previous` are
atomic symlinks. Production never builds from a source checkout.

## Build the artifact

From a Mac, run **Backend and Web Platform → Run workflow** in GitHub Actions
with **Build the deployable API, website and admin bundle** enabled. After all
checks pass, download `vlt-account-platform-<commit>` from the run artifacts.
Manual builds use `<app-version>-<short-commit>` as the deployment directory;
tagged builds use the tag version. The archive contains the API, migrations,
website, admin and deployment scripts from the same commit.

The release form's **Fill** template is stored in `admin/release-template.json`.
It provides both languages and all release-note sections. Save it as a draft
in Admin → Releases, then add installers later; do not publish an empty release.

On the Ubuntu CI runner, after tests pass:

```sh
bash packaging/server/build-bundle.sh 0.2.0
```

The command verifies generated API drift, builds the three Go binaries and two
standalone Next.js apps, then writes a tarball and sidecar SHA-256 file to
`dist/server/`. Upload both files. Do not upload source, build caches,
`node_modules`, `.env` files or user audio assets.

Tagged CI builds also receive a keyless GitHub build-provenance attestation.
Verify it before copying the archive to production (replace the repository):

```sh
gh attestation verify vlt-account-platform-0.2.0.tar.gz --repo OWNER/REPOSITORY
sha256sum -c vlt-account-platform-0.2.0.tar.gz.sha256
```

## Prepare the server once

Install PostgreSQL 18, Node.js 24, Apache, `curl` and PostgreSQL client tools.
Create a non-login `vltaccount` system user. Copy the tarball and its `.sha256`
file to the server, then install without deploying:

```sh
sudo bash packaging/server/install.sh vlt-account-platform-0.2.0.tar.gz --no-deploy
sudo cp /etc/vlt-account/api.env.example /etc/vlt-account/api.env
sudo chown root:root /etc/vlt-account/api.env
sudo chmod 0600 /etc/vlt-account/api.env
```

Replace every `CHANGE_ME`. Keep `COLLABORATION_ENABLED=false` and
`COLLAB_RECORDING_ENABLED=false` for the first smoke test. Never commit the
real file. The deploy tools and systemd both read it, so single-quote values
that contain spaces or shell metacharacters.

Copy `/etc/vlt-account/apache-vhost.example.conf` to an Apache site, replace
domains/certificate paths, enable `headers proxy proxy_http proxy_wstunnel
rewrite ssl`, obtain certificates, and run `apachectl configtest`.
Apply `s3-lifecycle.example.json` to the collaboration bucket through the
provider's lifecycle API; it expires abandoned staging objects and incomplete
multipart uploads after seven days, but never applies to the `blobs/` prefix.

## Deploy and roll back

```sh
sudo /opt/vlt-account-platform/releases/0.2.0/ops/deploy.sh \
  /opt/vlt-account-platform/releases/0.2.0
```

Deploy validates checksums/config/dependencies, applies only forward embedded
migrations, switches `current`, restarts services and runs local plus HTTPS
smoke tests. A failed smoke test restores the previous application symlink;
database down-migrations are never run.

After the acceptance test, set `COLLABORATION_ENABLED=true` and deploy/restart,
then grant individual accounts online access from **Admin → Users**. The
database entitlement defaults to false. `COLLAB_ALLOWED_USER_IDS` is only an
emergency OR override and should normally stay empty. Enable
`COLLAB_RECORDING_ENABLED=true` for the v6 recording workflow after validating
storage transfers and recording permissions on an isolated database.

If Apache injects analytics, exclude `/join`, `/en/join` and `/ru/join` using
`packaging/server/apache/vlt-metrika.conf`. The matching loader also skips invitation pages
and removes URL fragments from ordinary page hits. Verify the public endpoint,
because checks against the standalone Next.js server bypass Apache's filters.

To explicitly return to the prior application version:

```sh
sudo /opt/vlt-account-platform/current/ops/rollback.sh
```

Back up PostgreSQL and the S3 bucket before every production migration. A
rollback retains forward-compatible schema changes, so migrations must remain
expand-only until every deployed client no longer needs the old shape.

The release contains guarded `backup.sh`, `restore.sh`, and
`reset-collaboration.sh` tools. The reset tool requires the API to be stopped,
collaboration to be disabled, and explicit matching database/bucket names; it
removes only collaboration rows plus the `uploads/` and `blobs/` object
prefixes. It never touches accounts, auth, releases, or installers.

Typical first-rollout sequence (with all services stopped) is:

```sh
sudo /opt/vlt-account-platform/current/ops/backup.sh /var/backups/vlt/pre-v1
sudo /opt/vlt-account-platform/current/ops/reset-collaboration.sh \
  --database vltstudio --bucket vlt-production \
  --confirm RESET-COLLABORATION-DOGFOOD
```

Test `restore.sh` against an isolated PostgreSQL database and S3 test bucket
before production rollout. Its explicit database/bucket arguments and
confirmation phrase must match the backup manifest and live configuration.

Run `ops/collaboration-synthetic.sh` from external monitoring on its own
schedule. It uploads, reads, verifies, and removes one random object below the
collaboration-owned `uploads/synthetic/` prefix and checks that bucket
lifecycle configuration is readable. Do not make the account API service or
`/readyz` depend on this probe; alert collaboration operators when it fails.

Import `ops/alerts/vlt-collaboration.rules.yml` into Prometheus (or translate
the four expressions for the installed monitoring system). The rules page on
stuck session endings and storage failures, and warn on repeated snapshot
retries or a cleanup backlog. Scrape `/metrics` directly over loopback; the
handler intentionally rejects non-loopback clients and contains no user or
project labels.

## Download click statistics

Admin → Releases shows website download-button clicks, grouped by version and
installer format. The `releases.read` permission controls access. Clicks start
at deployment of migration 000034; historical traffic is not backfilled.
Repeated clicks count separately. These totals do not represent unique people
or completed transfers. Opening or cancelling the DMG warning does not count;
confirming its download does. File GET/Range requests and desktop updater traffic
are excluded. Replacing or removing an installer retains its version/format count.
The database stores aggregate counts and first/last click times, without visitor
identifiers or IP addresses. Website downloads continue if tracking is unavailable.

## Support link statistics

Admin → Overview shows Boosty link activations, split between the final homepage
support block and the shared footer. Migration 000036 stores aggregate counts and
timestamps only. Access requires `dashboard.read`. Counts start when deployed;
repeated, keyboard and middle-button activations count, while page views do not.
These are link clicks, not verified subscriptions or unique people. The external
link opens immediately even when the statistics API is unavailable.

## Forum and media worker

The bundle now includes `forum/` and `bin/vlt-forum-media`. Set `FORUM_ORIGIN=https://forum.vltstudio.ru`, configure DNS/TLS for the new Apache virtual host, and install both `vlt-account-forum.service` (127.0.0.1:3102) and `vlt-forum-media.service`. The API remains common to all three websites. The worker requires FFprobe/FFmpeg with H.264/AAC, VP9/Opus and JPEG encoders; preflight verifies them. It runs with CPU/memory/task limits. Choose a private **separate forum S3 bucket and credentials**, not the collaboration bucket. Apply `forum-s3-cors.example.json` with your exact forum origin and `forum-s3-lifecycle.example.json` (incomplete multipart/staging only; never expire published `objects/` or `media/`). No bucket public-read policy is needed; authorized downloads stream through Go with Range support.

Launch order: migrate → API and worker → forum → compatible desktop client. The existing install/deploy/smoke/rollback scripts include the new services. For the initial rollout, install the new unit files and Apache configuration before deployment; provision the TLS certificate before enabling the HTTPS host. Test login on both origins, a two-user discussion, direct upload and hide/restore before releasing the desktop build. Keep the previous release and a database/object snapshot; code rollback does not automatically reverse data migrations.

`/metrics` remains loopback-only and now exposes `vlt_forum_auth_failures_total`, `vlt_forum_upload_failures_total`, queue depth/age, active and expired leases, failed videos and DB collection errors. Import `alerts/vlt-forum.rules.yml` into Prometheus. API logs retain request IDs; worker logs are available with `journalctl -u vlt-forum-media`. Desktop installation errors are logged as `forum installation failed:` and shown in the browser; inspect them with the existing native diagnostic logs. Check both MP4 and WebM in browsers and deployed Qt builds, since Qt codec availability varies by build.

Backups include PostgreSQL and the forum `objects/`, `media/`, `staging/` prefixes. Stop API/media writes before running `backup.sh` to make the database and object snapshot consistent; the script verifies this. Restore with the services stopped, then migrate/start/smoke. Enable bucket versioning and keep off-server copies of both database and objects. Exercise restore into an isolated environment; a DB-only restore cannot recover attachments.

Implementation and local test commands: [forum/README.md](../../forum/README.md).
