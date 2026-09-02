package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"vltstudio/backend/internal/api"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
)

func main() {
	cfg, err := config.Load()
	if err != nil {
		log.Fatal(err)
	}
	db, err := database.Open(cfg.DatabaseURL, cfg.Environment == "development")
	if err != nil {
		log.Fatal(err)
	}
	var collaborationProcessLock *sql.Conn
	if cfg.CollaborationEnabled {
		lockContext, cancelLock := context.WithTimeout(context.Background(), 10*time.Second)
		collaborationProcessLock, err = acquireCollaborationProcessLock(lockContext, db)
		cancelLock()
		if err != nil {
			log.Fatal(err)
		}
		defer releaseCollaborationProcessLock(collaborationProcessLock)
	}
	server, err := api.New(cfg, db)
	if err != nil {
		log.Fatal(err)
	}
	httpServer := &http.Server{
		Addr: cfg.HTTPAddr, Handler: server.Router(),
		ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 90 * time.Second,
	}
	go func() {
		log.Printf("VLT API listening on %s (%s)", cfg.HTTPAddr, cfg.Environment)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatal(err)
		}
	}()
	stop, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	go server.RunCollaborationMaintenance(stop)
	<-stop.Done()
	server.ShutdownCollaboration()
	ctx, cancelShutdown := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancelShutdown()
	if err := httpServer.Shutdown(ctx); err != nil {
		log.Printf("shutdown: %v", err)
	}
}

const collaborationProcessLockName = "vlt-collaboration-api-v1-single-instance"

type sqlDatabaseProvider interface {
	DB() (*sql.DB, error)
}

// acquireCollaborationProcessLock pins one PostgreSQL session for the entire
// backend lifetime. Session advisory locks are deliberately used here (rather
// than transaction locks): a second API process must fail before it can accept
// collaboration traffic, while account-only deployments remain unaffected.
func acquireCollaborationProcessLock(ctx context.Context,
	db sqlDatabaseProvider) (*sql.Conn, error) {
	sqlDB, err := db.DB()
	if err != nil {
		return nil, fmt.Errorf("collaboration process lock database: %w", err)
	}
	connection, err := sqlDB.Conn(ctx)
	if err != nil {
		return nil, fmt.Errorf("collaboration process lock connection: %w", err)
	}
	var acquired bool
	err = connection.QueryRowContext(ctx,
		"SELECT pg_try_advisory_lock(hashtextextended($1, 0))",
		collaborationProcessLockName).Scan(&acquired)
	if err != nil {
		_ = connection.Close()
		return nil, fmt.Errorf("collaboration process lock: %w", err)
	}
	if !acquired {
		_ = connection.Close()
		return nil, errors.New("another collaboration API process already holds the process lock")
	}
	return connection, nil
}

func releaseCollaborationProcessLock(connection *sql.Conn) {
	if connection == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var released bool
	if err := connection.QueryRowContext(ctx,
		"SELECT pg_advisory_unlock(hashtextextended($1, 0))",
		collaborationProcessLockName).Scan(&released); err != nil {
		log.Printf("release collaboration process lock: %v", err)
	} else if !released {
		log.Printf("release collaboration process lock: lock was not held")
	}
	if err := connection.Close(); err != nil {
		log.Printf("close collaboration process lock connection: %v", err)
	}
}
