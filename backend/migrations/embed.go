package migrations

import "embed"

// Files contains the immutable SQL migration history used by cmd/migrate.
//
//go:embed *.sql
var Files embed.FS
