package auth

import (
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestSignerScopesAndExpiry(t *testing.T) {
	now := time.Date(2026, 8, 23, 12, 0, 0, 0, time.UTC)
	signer := NewSigner(make([]byte, 32))
	want := Claims{Subject: uuid.New(), Scope: "offline", IssuedAt: now.Unix(), ExpiresAt: now.Add(72 * time.Hour).Unix()}
	token, err := signer.Sign(want)
	if err != nil {
		t.Fatal(err)
	}
	got, err := signer.Verify(token, "offline", now.Add(71*time.Hour))
	if err != nil || got.Subject != want.Subject {
		t.Fatalf("valid entitlement was rejected: %+v, %v", got, err)
	}
	if _, err := signer.Verify(token, "desktop", now); err == nil {
		t.Fatal("token was accepted for the wrong scope")
	}
	if _, err := signer.Verify(token, "offline", now.Add(72*time.Hour)); err == nil {
		t.Fatal("expired entitlement was accepted")
	}
}

func TestRandomTokenHash(t *testing.T) {
	a, err := RandomToken(32)
	if err != nil {
		t.Fatal(err)
	}
	b, err := RandomToken(32)
	if err != nil {
		t.Fatal(err)
	}
	if a == b || HashToken(a) == HashToken(b) {
		t.Fatal("independent random tokens collided")
	}
}
