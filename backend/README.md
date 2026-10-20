# VLTONE backend

## Website visits

Apply migration `000037_site_visits` before deploying the API, public web app and
admin app together. The admin **Посещения** section (`/analytics`) requires
`analytics.read`; owners have access automatically, and owners can grant it in
**Команда**. Data starts at deployment; previous visits are not reconstructed.

The public site records visible page opens and client-side path changes using
`POST /v1/site-visits`. Unique event IDs prevent duplicate inserts. The API
checks Origin, limits collection to 120 requests per IP/minute, and ignores
known bots, DNT and GPC. Statistics use Moscow calendar days and include today,
1/7/30-day history, popular pages and a paginated visit log. Names come only from
validated account sessions; account deletion removes the account link.

Anonymous daily visitors are an approximate HMAC of the trusted client IP and
user agent, keyed with a domain-separated signing seed and the Moscow date.
Shared networks/browsers can merge people; changing networks can split them.
Configure `TRUSTED_PROXY_CIDRS` for the deployed reverse proxy so its own address
is not mistaken for visitors. No analytics cookies or browser identifiers are
created; raw IPs, full user agents, query strings and fragments are not stored.
Referrers retain only their hostname. The public privacy description documents
these records. The statistics page never exposes these details publicly.

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
