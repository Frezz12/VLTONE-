# VLT account platform — immutable Ubuntu deployment

Production runs one API process and two standalone Next.js processes behind
Apache. Releases are prebuilt in CI and installed under
`/opt/vlt-account-platform/releases/<version>`; `current` and `previous` are
atomic symlinks. Production never builds from a source checkout.

## Build the artifact

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
emergency OR override and should normally stay empty. Cloud recording must
remain false for V1.

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
