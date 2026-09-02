package api

import (
	"testing"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

func TestSignDesktopSessionTokensBindsExactSessionIdentity(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	user := model.User{ID: uuid.New(), ConsentVersion: "2026-08-23"}
	device := model.Device{ID: uuid.New(), UserID: user.ID}
	session := model.DesktopSession{
		ID: uuid.New(), UserID: user.ID, DeviceID: device.ID,
	}
	server := &Server{Signer: auth.NewSigner(make([]byte, 32))}
	tokens, err := server.signDesktopSessionTokens(user, device, session, now)
	if err != nil {
		t.Fatal(err)
	}
	access, err := server.Signer.Verify(tokens.AccessToken, "desktop", now)
	if err != nil {
		t.Fatalf("verify access token: %v", err)
	}
	offline, err := server.Signer.Verify(tokens.OfflineToken, "offline", now)
	if err != nil {
		t.Fatalf("verify offline entitlement: %v", err)
	}
	for _, claims := range []auth.Claims{access, offline} {
		if claims.Subject != user.ID || claims.DeviceID != device.ID ||
			claims.SessionID != session.ID || claims.ConsentVersion != user.ConsentVersion {
			t.Fatalf("signed claims escaped exact actor provenance: %#v", claims)
		}
	}
	if !tokens.AccessExpiresAt.Equal(now.Add(15*time.Minute)) ||
		!tokens.OfflineExpiresAt.Equal(now.Add(72*time.Hour)) ||
		!tokens.IssuedAt.Equal(now) {
		t.Fatalf("unexpected signed token times: %#v", tokens)
	}
}

func TestSignDesktopSessionTokensRejectsMismatchedRows(t *testing.T) {
	user := model.User{ID: uuid.New()}
	device := model.Device{ID: uuid.New(), UserID: user.ID}
	session := model.DesktopSession{
		ID: uuid.New(), UserID: user.ID, DeviceID: uuid.New(),
	}
	server := &Server{Signer: auth.NewSigner(make([]byte, 32))}
	if _, err := server.signDesktopSessionTokens(user, device, session,
		time.Now().UTC()); err == nil {
		t.Fatal("mismatched desktop session identity was signed")
	}
}
