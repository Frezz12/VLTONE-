package config

import (
	"encoding/base64"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	Environment          string
	HTTPAddr             string
	DatabaseURL          string
	PublicOrigin         string
	AdminOrigin          string
	DesktopAPIOrigin     string
	StorageRoot          string
	SigningSeed          []byte
	AICredentialsKey     []byte
	ConsentVersion       string
	AIEnabled            bool
	AIGlobalMonthlyLimit int64
	SMTPHost             string
	SMTPPort             int
	SMTPUsername         string
	SMTPPassword         string
	SMTPFrom             string
	TrustedProxyCIDRs    []string
}

func Load() (Config, error) {
	c := Config{
		Environment:          env("APP_ENV", "development"),
		HTTPAddr:             env("HTTP_ADDR", ":8080"),
		DatabaseURL:          env("DATABASE_URL", "postgres://vlt:vlt@localhost:5432/vltstudio?sslmode=disable"),
		PublicOrigin:         strings.TrimRight(env("PUBLIC_ORIGIN", "http://localhost:3000"), "/"),
		AdminOrigin:          strings.TrimRight(env("ADMIN_ORIGIN", "http://localhost:3001"), "/"),
		DesktopAPIOrigin:     strings.TrimRight(env("DESKTOP_API_ORIGIN", "http://localhost:8080"), "/"),
		StorageRoot:          env("STORAGE_ROOT", "./storage"),
		ConsentVersion:       env("TELEMETRY_CONSENT_VERSION", "2026-08-23"),
		AIEnabled:            boolEnv("AI_ENABLED", true),
		AIGlobalMonthlyLimit: int64Env("AI_GLOBAL_MONTHLY_TOKEN_LIMIT", 0),
		SMTPHost:             os.Getenv("SMTP_HOST"),
		SMTPPort:             int(int64Env("SMTP_PORT", 587)),
		SMTPUsername:         os.Getenv("SMTP_USERNAME"),
		SMTPPassword:         os.Getenv("SMTP_PASSWORD"),
		SMTPFrom:             env("SMTP_FROM", "VLT Studio <no-reply@example.com>"),
		TrustedProxyCIDRs:    csv(env("TRUSTED_PROXY_CIDRS", "")),
	}
	root, err := filepath.Abs(c.StorageRoot)
	if err != nil {
		return Config{}, fmt.Errorf("storage root: %w", err)
	}
	c.StorageRoot = root
	seedText := strings.TrimSpace(os.Getenv("AUTH_SIGNING_SEED"))
	if seedText != "" {
		c.SigningSeed, err = base64.StdEncoding.DecodeString(seedText)
		if err != nil || len(c.SigningSeed) != 32 {
			return Config{}, errors.New("AUTH_SIGNING_SEED must be a base64-encoded 32-byte seed")
		}
	}
	credentialsKeyText := strings.TrimSpace(os.Getenv("AI_CREDENTIALS_KEY"))
	if credentialsKeyText != "" {
		c.AICredentialsKey, err = base64.StdEncoding.DecodeString(credentialsKeyText)
		if err != nil || len(c.AICredentialsKey) != 32 {
			return Config{}, errors.New("AI_CREDENTIALS_KEY must be a base64-encoded 32-byte key")
		}
	}
	for _, raw := range []string{c.PublicOrigin, c.AdminOrigin, c.DesktopAPIOrigin} {
		parsed, parseErr := url.Parse(raw)
		if parseErr != nil || parsed.Scheme == "" || parsed.Host == "" {
			return Config{}, fmt.Errorf("invalid origin %q", raw)
		}
		if c.Environment == "production" && parsed.Scheme != "https" {
			return Config{}, fmt.Errorf("production origin must use HTTPS: %s", raw)
		}
	}
	if c.Environment == "production" {
		if len(c.SigningSeed) != 32 {
			return Config{}, errors.New("AUTH_SIGNING_SEED is required in production")
		}
		if c.AIGlobalMonthlyLimit <= 0 {
			return Config{}, errors.New("AI_GLOBAL_MONTHLY_TOKEN_LIMIT must be positive in production")
		}
		if c.SMTPHost == "" || c.SMTPPort <= 0 {
			return Config{}, errors.New("SMTP_HOST and a valid SMTP_PORT are required in production")
		}
	}
	return c, nil
}

func env(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

func int64Env(name string, fallback int64) int64 {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func boolEnv(name string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func csv(value string) []string {
	var out []string
	for _, item := range strings.Split(value, ",") {
		if item = strings.TrimSpace(item); item != "" {
			out = append(out, item)
		}
	}
	return out
}
