package config

import (
	"encoding/base64"
	"strings"
	"testing"
)

func TestProductionRequiresSMTPAndGlobalBudget(t *testing.T) {
	t.Setenv("APP_ENV", "production")
	t.Setenv("PUBLIC_ORIGIN", "https://vlt.example")
	t.Setenv("ADMIN_ORIGIN", "https://admin.vlt.example")
	t.Setenv("DESKTOP_API_ORIGIN", "https://api.vlt.example")
	t.Setenv("AUTH_SIGNING_SEED", base64.StdEncoding.EncodeToString(make([]byte, 32)))
	t.Setenv("AI_GLOBAL_MONTHLY_TOKEN_LIMIT", "100000000")
	t.Setenv("SMTP_HOST", "")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "SMTP_HOST") {
		t.Fatalf("production without SMTP was accepted: %v", err)
	}
	t.Setenv("SMTP_HOST", "smtp.vlt.example")
	if _, err := Load(); err != nil {
		t.Fatalf("valid production configuration was rejected: %v", err)
	}
}

func TestAICredentialsKeyMustBe32Bytes(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("AI_CREDENTIALS_KEY", base64.StdEncoding.EncodeToString([]byte("too short")))
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "AI_CREDENTIALS_KEY") {
		t.Fatalf("invalid AI credential key was accepted: %v", err)
	}

	t.Setenv("AI_CREDENTIALS_KEY", base64.StdEncoding.EncodeToString(make([]byte, 32)))
	if loaded, err := Load(); err != nil || len(loaded.AICredentialsKey) != 32 {
		t.Fatalf("valid AI credential key was rejected: key=%d err=%v", len(loaded.AICredentialsKey), err)
	}
}
