package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
	"golang.org/x/text/cases"
	"golang.org/x/text/unicode/norm"
)

const (
	argonMemory      = 19 * 1024
	argonIterations  = 2
	argonParallelism = 1
	argonSaltBytes   = 16
	argonKeyBytes    = 32
)

func NormalizeEmail(value string) string {
	return cases.Fold().String(norm.NFKC.String(strings.TrimSpace(value)))
}

func NormalizeNickname(value string) string {
	return cases.Fold().String(norm.NFKC.String(strings.TrimSpace(value)))
}

func ValidatePassword(value string) error {
	count := len([]rune(value))
	if count < 12 || count > 128 {
		return errors.New("password must contain between 12 and 128 characters")
	}
	return nil
}

// ValidateSessionSecret is deliberately laxer than ValidatePassword. A session
// password is a short-lived secret shared out loud between people who are about
// to work together, protecting one ephemeral room rather than an account, and a
// twelve character floor would just push everyone to reuse an account password.
func ValidateSessionSecret(value string) error {
	count := len([]rune(value))
	if count < 6 || count > 128 {
		return errors.New("session password must contain between 6 and 128 characters")
	}
	return nil
}

func HashPassword(password string) (string, error) {
	if err := ValidatePassword(password); err != nil {
		return "", err
	}
	return hashWithArgon(password)
}

// HashSessionSecret uses the same argon2id parameters as HashPassword; only the
// length policy differs. VerifyPassword reads both, since it applies no policy
// of its own and is already constant time.
func HashSessionSecret(value string) (string, error) {
	if err := ValidateSessionSecret(value); err != nil {
		return "", err
	}
	return hashWithArgon(value)
}

func hashWithArgon(value string) (string, error) {
	salt := make([]byte, argonSaltBytes)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	hash := argon2.IDKey([]byte(value), salt, argonIterations, argonMemory,
		argonParallelism, argonKeyBytes)
	return fmt.Sprintf("$argon2id$v=19$m=%d,t=%d,p=%d$%s$%s", argonMemory,
		argonIterations, argonParallelism,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(hash)), nil
}

func VerifyPassword(encoded, password string) bool {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "argon2id" || parts[2] != "v=19" {
		return false
	}
	var memory uint32
	var iterations uint32
	var parallelism uint8
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &memory, &iterations, &parallelism); err != nil {
		return false
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return false
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(want) == 0 {
		return false
	}
	got := argon2.IDKey([]byte(password), salt, iterations, memory, parallelism,
		uint32(len(want)))
	return subtle.ConstantTimeCompare(got, want) == 1
}
