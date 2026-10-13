package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/forumfiles"
)

func main() {
	cfg, e := config.Load()
	if e != nil {
		log.Fatal(e)
	}
	db, e := database.Open(cfg.DatabaseURL, false)
	if e != nil {
		log.Fatal(e)
	}
	files, e := forumfiles.New(cfg)
	if e != nil {
		log.Fatal(e)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	(&forumfiles.Worker{DB: db, Files: files, Config: cfg}).Run(ctx)
}
