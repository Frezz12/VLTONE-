package migrations

import (
	"context"
	"database/sql"
	"fmt"
	"sort"
	"strings"
)

const ensureTable = `CREATE TABLE IF NOT EXISTS schema_migrations (
    version text PRIMARY KEY, applied_at timestamptz NOT NULL DEFAULT now())`

func names(suffix string) ([]string, error) {
	entries, err := Files.ReadDir(".")
	if err != nil {
		return nil, err
	}
	var result []string
	for _, entry := range entries {
		if strings.HasSuffix(entry.Name(), suffix) {
			result = append(result, entry.Name())
		}
	}
	sort.Strings(result)
	return result, nil
}

func version(name string) string { return strings.SplitN(name, "_", 2)[0] }

// Up applies every embedded, versioned migration transactionally.
func Up(ctx context.Context, db *sql.DB) error {
	if _, err := db.ExecContext(ctx, ensureTable); err != nil {
		return err
	}
	files, err := names(".up.sql")
	if err != nil {
		return err
	}
	for _, name := range files {
		var exists bool
		if err := db.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version=$1)", version(name)).Scan(&exists); err != nil {
			return err
		}
		if exists {
			continue
		}
		body, err := Files.ReadFile(name)
		if err != nil {
			return err
		}
		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, string(body)); err == nil {
			_, err = tx.ExecContext(ctx, "INSERT INTO schema_migrations(version) VALUES($1)", version(name))
		}
		if err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("apply %s: %w", name, err)
		}
		if err := tx.Commit(); err != nil {
			return err
		}
	}
	return nil
}

// Down rolls back the newest applied migration. Calling it repeatedly reaches
// the empty schema and is used by CI to verify both directions.
func Down(ctx context.Context, db *sql.DB) error {
	if _, err := db.ExecContext(ctx, ensureTable); err != nil {
		return err
	}
	var current string
	if err := db.QueryRowContext(ctx, "SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1").Scan(&current); err != nil {
		if err == sql.ErrNoRows {
			return nil
		}
		return err
	}
	files, err := names(".down.sql")
	if err != nil {
		return err
	}
	for _, name := range files {
		if version(name) != current {
			continue
		}
		body, err := Files.ReadFile(name)
		if err != nil {
			return err
		}
		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, string(body)); err == nil {
			_, err = tx.ExecContext(ctx, "DELETE FROM schema_migrations WHERE version=$1", current)
		}
		if err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("rollback %s: %w", name, err)
		}
		return tx.Commit()
	}
	return fmt.Errorf("down migration for %s was not found", current)
}
