package collab

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

func TestCheckSessionSecret(t *testing.T) {
	hash, err := auth.HashSessionSecret("jam night")
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	protected := model.ProjectSession{PasswordHash: &hash}
	open := model.ProjectSession{}

	// An unprotected session admits anyone, with or without a secret, so
	// adding the field to the request never changes existing behaviour.
	if err := checkSessionSecret(open, SessionSecret{}); err != nil {
		t.Fatalf("open session rejected an empty secret: %v", err)
	}
	if err := checkSessionSecret(open, SessionSecret{Password: "anything"}); err != nil {
		t.Fatalf("open session rejected a stray secret: %v", err)
	}

	if err := checkSessionSecret(protected, SessionSecret{}); !errors.Is(err, ErrSessionPasswordRequired) {
		t.Fatalf("empty secret on a protected session: %v", err)
	}
	if err := checkSessionSecret(protected, SessionSecret{Password: "wrong"}); !errors.Is(err, ErrSessionPasswordInvalid) {
		t.Fatalf("wrong secret on a protected session: %v", err)
	}
	if err := checkSessionSecret(protected, SessionSecret{Password: "jam night"}); err != nil {
		t.Fatalf("correct secret was rejected: %v", err)
	}
}

func TestSessionSecretLengthPolicyIsLooserThanAccountPasswords(t *testing.T) {
	// A room password is shared out loud between people about to work together
	// and protects one ephemeral session. Requiring the account floor of twelve
	// characters would just push everyone to reuse their account password.
	if err := auth.ValidateSessionSecret("jam123"); err != nil {
		t.Fatalf("a six character room password was rejected: %v", err)
	}
	if err := auth.ValidatePassword("jam123"); err == nil {
		t.Fatal("the account policy accepted six characters; this test is stale")
	}
	if err := auth.ValidateSessionSecret("short"); err == nil {
		t.Fatal("a five character room password was accepted")
	}
	if err := auth.ValidateSessionSecret(strings.Repeat("x", 129)); err == nil {
		t.Fatal("an overlong room password was accepted")
	}

	// It still hashes with argon2id and verifies through the same path.
	hash, err := auth.HashSessionSecret("jam123")
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	if !strings.HasPrefix(hash, "$argon2id$") {
		t.Fatalf("session secret is not argon2id: %q", hash)
	}
	if !auth.VerifyPassword(hash, "jam123") || auth.VerifyPassword(hash, "jam124") {
		t.Fatal("session secret does not verify through VerifyPassword")
	}
}

// The store serialises these structs straight to the desktop client. A secret
// that reaches the wire is the whole failure mode, so assert on the bytes
// rather than trusting the struct tags to stay correct.
func TestSessionAndInviteSecretsNeverSerialize(t *testing.T) {
	hash, err := auth.HashSessionSecret("jam night")
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	now := time.Now().UTC()
	lookup := "b0a1" + strings.Repeat("c", 60)
	locked := now.Add(time.Minute)

	session := model.ProjectSession{
		ID: uuid.New(), ProjectID: uuid.New(), Mode: "independent",
		Status: "active", Version: 1, CreatedAt: now, UpdatedAt: now,
		PasswordHash: &hash, PasswordSetAt: &now,
	}
	invite := model.ProjectInvite{
		ID: uuid.New(), ProjectID: uuid.New(), Role: "editor",
		TokenHash: strings.Repeat("a", 64), ExpiresAt: now, CreatedAt: now,
		CodeLookup: &lookup, CodeDigits: InviteCodeDigits,
		AttemptCount: 7, LockedUntil: &locked,
	}

	for name, value := range map[string]any{"session": session, "invite": invite} {
		encoded, err := json.Marshal(value)
		if err != nil {
			t.Fatalf("marshal %s: %v", name, err)
		}
		text := string(encoded)
		for _, forbidden := range []string{
			hash, lookup, strings.Repeat("a", 64),
			"password_hash", "passwordHash", "PasswordHash",
			"code_lookup", "codeLookup", "CodeLookup",
			"token_hash", "tokenHash", "TokenHash",
			"attempt_count", "locked_until",
		} {
			if strings.Contains(text, forbidden) {
				t.Fatalf("%s payload leaks %q: %s", name, forbidden, text)
			}
		}
	}

	// The one derived fact a client is allowed to learn.
	if !session.PasswordRequired() {
		t.Fatal("a protected session did not report PasswordRequired")
	}
	if (model.ProjectSession{}).PasswordRequired() {
		t.Fatal("an open session reported PasswordRequired")
	}
	// code_digits is safe and useful: it tells the join dialog how wide the
	// input should be without revealing anything about the code.
	encoded, err := json.Marshal(invite)
	if err != nil {
		t.Fatalf("marshal invite: %v", err)
	}
	if !strings.Contains(string(encoded), "code_digits") {
		t.Fatalf("invite payload lost code_digits: %s", encoded)
	}
}
