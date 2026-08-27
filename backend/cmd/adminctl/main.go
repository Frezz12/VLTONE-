package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	"github.com/google/uuid"
	"golang.org/x/term"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

func main() {
	if len(os.Args) < 2 || os.Args[1] != "create-owner" {
		log.Fatal("usage: go run ./cmd/adminctl create-owner --email owner@example.com --nickname Owner")
	}
	flags := flag.NewFlagSet("create-owner", flag.ExitOnError)
	email := flags.String("email", "", "owner email")
	nickname := flags.String("nickname", "Owner", "owner display name")
	_ = flags.Parse(os.Args[2:])
	if auth.NormalizeEmail(*email) == "" {
		log.Fatal("--email is required")
	}
	password := os.Getenv("VLT_ADMIN_PASSWORD")
	if password == "" {
		fmt.Fprint(os.Stderr, "Owner password: ")
		body, err := term.ReadPassword(int(os.Stdin.Fd()))
		fmt.Fprintln(os.Stderr)
		if err != nil {
			log.Fatal(err)
		}
		password = string(body)
	}
	hash, err := auth.HashPassword(password)
	if err != nil {
		log.Fatal(err)
	}
	cfg, err := config.Load()
	if err != nil {
		log.Fatal(err)
	}
	db, err := database.Open(cfg.DatabaseURL, cfg.Environment == "development")
	if err != nil {
		log.Fatal(err)
	}
	admin := model.AdminUser{
		ID: uuid.New(), Email: strings.TrimSpace(*email), EmailKey: auth.NormalizeEmail(*email),
		Nickname: strings.TrimSpace(*nickname), PasswordHash: hash, Status: model.UserActive,
		CreatedAt: time.Now().UTC(), UpdatedAt: time.Now().UTC(),
	}
	if err := db.Create(&admin).Error; err != nil {
		log.Fatal(err)
	}
	log.Printf("created owner %s", admin.Email)
}
