package main

import (
	"context"
	"database/sql"
	"log"
	"os"

	_ "github.com/jackc/pgx/v5/stdlib"

	"vltstudio/backend/internal/config"
	"vltstudio/backend/migrations"
)

func main() {
	if len(os.Args) != 2 || (os.Args[1] != "up" && os.Args[1] != "down") {
		log.Fatal("usage: go run ./cmd/migrate up|down")
	}
	cfg, err := config.Load()
	if err != nil {
		log.Fatal(err)
	}
	db, err := sql.Open("pgx", cfg.DatabaseURL)
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()
	ctx := context.Background()
	if os.Args[1] == "up" {
		err = migrations.Up(ctx, db)
	} else {
		err = migrations.Down(ctx, db)
	}
	if err != nil {
		log.Fatal(err)
	}
}
