package main

import (
	"context"
	"database/sql"
	"os"
	"strings"
	"testing"
	"time"

	"vltstudio/backend/internal/database"
)

func TestCollaborationProcessLockRejectsSecondBackend(t *testing.T) {
	dsn := strings.TrimSpace(os.Getenv("VLT_COLLAB_TEST_DATABASE_URL"))
	if dsn == "" {
		if os.Getenv("CI") != "" {
			t.Fatal("VLT_COLLAB_TEST_DATABASE_URL is required in CI")
		}
		t.Skip("VLT_COLLAB_TEST_DATABASE_URL is required by the collaboration CI job")
	}

	firstDatabase, err := database.Open(dsn, false)
	if err != nil {
		t.Fatalf("open first database: %v", err)
	}
	secondDatabase, err := database.Open(dsn, false)
	if err != nil {
		t.Fatalf("open second database: %v", err)
	}
	for _, handle := range []interface{ DB() (*sql.DB, error) }{firstDatabase, secondDatabase} {
		sqlDatabase, databaseError := handle.DB()
		if databaseError != nil {
			t.Fatalf("database handle: %v", databaseError)
		}
		t.Cleanup(func() { _ = sqlDatabase.Close() })
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	firstLock, err := acquireCollaborationProcessLock(ctx, firstDatabase)
	if err != nil {
		t.Fatalf("acquire first lock: %v", err)
	}
	t.Cleanup(func() { releaseCollaborationProcessLock(firstLock) })

	if secondLock, secondError := acquireCollaborationProcessLock(ctx, secondDatabase); secondError == nil {
		releaseCollaborationProcessLock(secondLock)
		t.Fatal("second collaboration backend acquired the process lock")
	}

	releaseCollaborationProcessLock(firstLock)
	firstLock = nil
	secondLock, err := acquireCollaborationProcessLock(ctx, secondDatabase)
	if err != nil {
		t.Fatalf("lock was not released for a replacement backend: %v", err)
	}
	releaseCollaborationProcessLock(secondLock)
}
