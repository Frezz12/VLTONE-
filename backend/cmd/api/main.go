package main

import (
	"context"
	"errors"
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
	<-stop.Done()
	ctx, cancelShutdown := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancelShutdown()
	if err := httpServer.Shutdown(ctx); err != nil {
		log.Printf("shutdown: %v", err)
	}
}
