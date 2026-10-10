# VLTONE backend

The Go service is the only business backend for the public site, admin site and
desktop DAW. It expects a local PostgreSQL 18 instance; Docker is deliberately
not part of the development workflow.

```bash
cp .env.example .env
set -a; source .env; set +a
go run ./cmd/migrate up
go run ./cmd/adminctl create-owner --email owner@example.com --nickname Owner
go run ./cmd/api
```

Configuration is read from environment variables. The service never runs ORM
auto-migrations. Every schema change belongs in `migrations/` and is applied by
`cmd/migrate`.

Local endpoints:

- API: `http://localhost:8080`
- health: `GET /healthz`
- OpenAPI contract: `openapi/openapi.yaml`

In development a missing SMTP configuration prints password-reset links and
crash alerts to the server terminal. In production, each stored crash report is
emailed to every active administrator with crash-view permission. Production refuses to start without
HTTPS origins, an explicit Ed25519 signing seed and a positive global AI token
budget.

Run the unit suite with `go test ./...`. PostgreSQL integration coverage is
enabled by `VLT_TEST_DATABASE_URL`; it resets that dedicated database schema and
checks both migration directions, so never point it at a database containing
real data:

```bash
VLT_TEST_DATABASE_URL='postgres://vlt_test:vlt_test@localhost:5432/vlt_test?sslmode=disable' go test ./...
```

The fixed toolchain is Go 1.26.7 (`toolchain` in `go.mod`), GORM 1.31.2 and
PostgreSQL 18.6. Reporter credentials cover the same 72-hour window as the
offline entitlement and rotate whenever the DAW refreshes its desktop session.

## Administrator team and task board

Migration `000032_admin_team_tasks` preserves existing administrators as owners.
Run `go run ./cmd/migrate up` before deploying the updated API and admin site.
New owners are still created with `adminctl create-owner`. Only owners can grant
or revoke administrator access in **Команда**. Enter the email of an existing,
active website account and select read/write permissions for each section.
Write access requires read access and includes that section's destructive
operations; user write access includes account deletion, suspension and quotas.
Permissions are checked on every API request. Changes to a grant revoke that
member's current admin sessions. Linked members always use their current website
password; password resets and account-wide session revocation end their admin
sessions too. Existing owner credentials remain valid.

**Доска задач** requires `tasks.read`; editing, comments and attachments require
`tasks.write`. The board supports ideas, planned work, work in progress, review,
done and an archive. Cards retain their author, assignee, priority, due date and
comments. Saving uses a version check to prevent concurrent edits overwriting
each other. Lists are paginated in batches of 100; the UI can load subsequent
batches. Files are private authenticated downloads, limited to 10 MiB each and
20 per task. Attachment bytes live in PostgreSQL and are covered by database
backups. Task and access mutations are recorded in the existing admin audit log.
