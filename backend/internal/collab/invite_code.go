package collab

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"strings"

	"github.com/jackc/pgx/v5/pgconn"
	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
)

// NormalizeInviteCode strips everything that is not a digit, so a code pasted
// as "1234 5678 9012", "1234-5678-9012" or with a stray newline all reduce to
// the same value. It deliberately does not accept a prefix: the caller checks
// the length against the digit count recorded on the invite.
func NormalizeInviteCode(value string) string {
	var out strings.Builder
	out.Grow(len(value))
	for _, r := range value {
		if r >= '0' && r <= '9' {
			out.WriteRune(r)
		}
	}
	return out.String()
}

// ValidInviteCode reports whether `normalized` is exactly the expected number
// of digits. Anything else is refused before it reaches the database.
func ValidInviteCode(normalized string) bool {
	if len(normalized) != InviteCodeDigits {
		return false
	}
	for index := range len(normalized) {
		if normalized[index] < '0' || normalized[index] > '9' {
			return false
		}
	}
	return true
}

// generateInviteCode draws InviteCodeDigits uniformly.
//
// Rejection sampling rather than `% 10`: the modulo of a uniform 64-bit draw is
// biased toward the low digits, which is exactly the structure an attacker
// would exploit when ordering guesses. Leading zeros are kept — the code is a
// fixed-width string, not a number.
func generateInviteCode() (string, error) {
	const limit = ^uint64(0) - (^uint64(0) % 10)
	digits := make([]byte, InviteCodeDigits)
	var buffer [8]byte
	for index := range digits {
		for {
			if _, err := rand.Read(buffer[:]); err != nil {
				return "", err
			}
			draw := binary.BigEndian.Uint64(buffer[:])
			if draw >= limit {
				continue // would bias the result; draw again
			}
			digits[index] = byte('0' + draw%10)
			break
		}
	}
	return string(digits), nil
}

// inviteCodeLookup derives the stored, indexable form of a code.
func (s *Store) inviteCodeLookup(code string) (string, error) {
	if len(s.InviteCodePepper) == 0 {
		return "", errors.New("invite codes are not configured on this server")
	}
	return auth.HashLowEntropyCode(s.InviteCodePepper, inviteCodeDomain, code)
}

// InviteCodesEnabled reports whether this deployment can mint and redeem codes.
// Without a pepper the long-token path still works, so the feature degrades
// instead of handing out codes that a database disclosure would reveal.
func (s *Store) InviteCodesEnabled() bool {
	return len(s.InviteCodePepper) >= auth.MinimumCodePepperBytes
}

// FormatInviteCode groups a code for display: 1234 5678 9012. Presentation
// only; every stored and transmitted form is bare digits.
func FormatInviteCode(code string) string {
	var out strings.Builder
	for index := range len(code) {
		if index > 0 && index%4 == 0 {
			out.WriteByte(' ')
		}
		out.WriteByte(code[index])
	}
	return out.String()
}

// isUniqueViolation recognises a duplicate-key error from either the GORM
// translation layer or the raw driver, so the caller works whether or not
// TranslateError is enabled on the session.
func isUniqueViolation(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, gorm.ErrDuplicatedKey) {
		return true
	}
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		return pgErr.Code == "23505"
	}
	return false
}
