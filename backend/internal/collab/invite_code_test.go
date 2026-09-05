package collab

import (
	"strings"
	"testing"

	"vltstudio/backend/internal/auth"
)

var testPepper = []byte("0123456789abcdef0123456789abcdef")

func TestInviteCodeGenerationIsWellFormedAndUnbiased(t *testing.T) {
	const draws = 4000
	seen := make(map[string]struct{}, draws)
	counts := [10]int{}
	for range draws {
		code, err := generateInviteCode()
		if err != nil {
			t.Fatalf("generate: %v", err)
		}
		if !ValidInviteCode(code) {
			t.Fatalf("generated code is not valid: %q", code)
		}
		if _, duplicate := seen[code]; duplicate {
			t.Fatalf("generated a duplicate code within %d draws: %q", draws, code)
		}
		seen[code] = struct{}{}
		for index := range len(code) {
			counts[code[index]-'0']++
		}
	}
	// Rejection sampling should keep every digit near 1/10 of the total. A
	// modulo-based generator skews the low digits, which is exactly the
	// structure an attacker would use to order guesses, so this bound is the
	// point of the test rather than incidental.
	total := draws * InviteCodeDigits
	for digit, count := range counts {
		share := float64(count) / float64(total)
		if share < 0.085 || share > 0.115 {
			t.Fatalf("digit %d appeared %.4f of the time, expected about 0.1",
				digit, share)
		}
	}
}

func TestInviteCodeGenerationKeepsLeadingZeros(t *testing.T) {
	// A code is a fixed-width string, not a number. If any path ever treated it
	// as an integer the leading zeros would vanish and the code would stop
	// matching, so pin the width rather than the value.
	for range 500 {
		code, err := generateInviteCode()
		if err != nil {
			t.Fatalf("generate: %v", err)
		}
		if len(code) != InviteCodeDigits {
			t.Fatalf("code width drifted: %q", code)
		}
	}
}

func TestNormalizeInviteCodeAcceptsHumanSpellings(t *testing.T) {
	for _, spelling := range []string{
		"123456789012",
		"1234 5678 9012",
		"1234-5678-9012",
		" 1234\t5678\n9012 ",
		"(1234) 5678.9012",
	} {
		if got := NormalizeInviteCode(spelling); got != "123456789012" {
			t.Fatalf("normalize(%q) = %q", spelling, got)
		}
	}
}

func TestValidInviteCodeRejectsWrongLengthAndNonDigits(t *testing.T) {
	if !ValidInviteCode("123456789012") {
		t.Fatal("a well formed code was rejected")
	}
	for _, bad := range []string{
		"",
		"12345678901",   // one short
		"1234567890123", // one long
		"12345678901a",  // normalization would have stripped this, but the
		// validator must not depend on having been called after it
	} {
		if ValidInviteCode(bad) {
			t.Fatalf("invalid code was accepted: %q", bad)
		}
	}
}

func TestHashLowEntropyCodeIsDeterministicAndKeyed(t *testing.T) {
	const code = "123456789012"
	first, err := auth.HashLowEntropyCode(testPepper, "invite-code-v1", code)
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	again, err := auth.HashLowEntropyCode(testPepper, "invite-code-v1", code)
	if err != nil || again != first {
		t.Fatalf("hash is not deterministic: %v %q %q", err, first, again)
	}
	if len(first) != 64 {
		t.Fatalf("hash is not a sha256 hex digest: %q", first)
	}
	if strings.Contains(first, code) {
		t.Fatalf("hash leaks the code: %q", first)
	}

	// A different pepper must not produce the same lookup value, or rotating it
	// would leave old codes redeemable.
	other := append([]byte(nil), testPepper...)
	other[0] ^= 0xff
	rotated, err := auth.HashLowEntropyCode(other, "invite-code-v1", code)
	if err != nil || rotated == first {
		t.Fatalf("hash is not keyed by the pepper: %v", err)
	}

	// Domain separation: the same code under another use must not collide.
	domained, err := auth.HashLowEntropyCode(testPepper, "some-other-use", code)
	if err != nil || domained == first {
		t.Fatalf("hash is not domain separated: %v", err)
	}
}

func TestHashLowEntropyCodeRefusesAWeakPepper(t *testing.T) {
	// Fail closed: silently hashing with a short key would hand every
	// outstanding invite to anyone who read the database.
	short := make([]byte, auth.MinimumCodePepperBytes-1)
	if _, err := auth.HashLowEntropyCode(short, "invite-code-v1", "123456789012"); err == nil {
		t.Fatal("a short pepper was accepted")
	}
	if _, err := auth.HashLowEntropyCode(testPepper, "", "123456789012"); err == nil {
		t.Fatal("an empty domain was accepted")
	}
	if _, err := auth.HashLowEntropyCode(testPepper, "invite-code-v1", ""); err == nil {
		t.Fatal("an empty code was accepted")
	}
}

func TestInviteCodesEnabledFollowsThePepper(t *testing.T) {
	store := &Store{}
	if store.InviteCodesEnabled() {
		t.Fatal("codes are enabled without a pepper")
	}
	if _, err := store.inviteCodeLookup("123456789012"); err == nil {
		t.Fatal("a lookup was derived without a pepper")
	}
	store.InviteCodePepper = testPepper
	if !store.InviteCodesEnabled() {
		t.Fatal("codes are disabled with a valid pepper")
	}
}

func TestFormatInviteCodeGroupsForReading(t *testing.T) {
	if got := FormatInviteCode("123456789012"); got != "1234 5678 9012" {
		t.Fatalf("format = %q", got)
	}
	// Grouping is presentation only and must round-trip back to the wire form.
	if got := NormalizeInviteCode(FormatInviteCode("123456789012")); got != "123456789012" {
		t.Fatalf("format did not round-trip: %q", got)
	}
}
