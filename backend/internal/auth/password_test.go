package auth

import (
	"strings"
	"testing"
)

func TestNormalizeIdentity(t *testing.T) {
	if got := NormalizeEmail("  TEST@Example.COM "); got != "test@example.com" {
		t.Fatalf("NormalizeEmail() = %q", got)
	}
	// Full-width Unicode is folded through NFKC before the case-insensitive key
	// is created; the original nickname remains untouched in the user row.
	if got := NormalizeNickname("  ＶＬＴТест  "); got != "vltтест" {
		t.Fatalf("NormalizeNickname() = %q", got)
	}
}

func TestArgon2idPasswordRoundTrip(t *testing.T) {
	hash, err := HashPassword("correct horse battery staple")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(hash, "$argon2id$v=19$m=19456,t=2,p=1$") {
		t.Fatalf("unexpected password parameters: %s", hash)
	}
	if !VerifyPassword(hash, "correct horse battery staple") {
		t.Fatal("valid password was rejected")
	}
	if VerifyPassword(hash, "incorrect horse battery staple") {
		t.Fatal("invalid password was accepted")
	}
}

func TestPasswordLengthUsesCharacters(t *testing.T) {
	if ValidatePassword("короткийпар!") != nil {
		t.Fatal("12-character Unicode password was rejected")
	}
	if ValidatePassword("too-short") == nil {
		t.Fatal("short password was accepted")
	}
	if ValidatePassword(strings.Repeat("я", 129)) == nil {
		t.Fatal("overlong password was accepted")
	}
}
