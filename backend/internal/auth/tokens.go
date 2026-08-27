package auth

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/google/uuid"
)

type Claims struct {
	Subject        uuid.UUID `json:"sub"`
	Scope          string    `json:"scope"`
	DeviceID       uuid.UUID `json:"device_id,omitempty"`
	SessionID      uuid.UUID `json:"session_id,omitempty"`
	Plan           string    `json:"plan,omitempty"`
	ConsentVersion string    `json:"consent_version,omitempty"`
	IssuedAt       int64     `json:"iat"`
	ExpiresAt      int64     `json:"exp"`
}

type Signer struct {
	private ed25519.PrivateKey
	public  ed25519.PublicKey
}

func NewSigner(seed []byte) *Signer {
	if len(seed) != ed25519.SeedSize {
		digest := sha256.Sum256([]byte("VLT Studio development signing key; never use in production"))
		seed = digest[:]
	}
	private := ed25519.NewKeyFromSeed(seed)
	return &Signer{private: private, public: private.Public().(ed25519.PublicKey)}
}

func (s *Signer) PublicKeyBase64() string {
	return base64.RawStdEncoding.EncodeToString(s.public)
}

func (s *Signer) Sign(claims Claims) (string, error) {
	header, _ := json.Marshal(map[string]string{"alg": "EdDSA", "typ": "VLT"})
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	unsigned := base64.RawURLEncoding.EncodeToString(header) + "." +
		base64.RawURLEncoding.EncodeToString(payload)
	signature := ed25519.Sign(s.private, []byte(unsigned))
	return unsigned + "." + base64.RawURLEncoding.EncodeToString(signature), nil
}

func (s *Signer) Verify(token, scope string, now time.Time) (Claims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return Claims{}, errors.New("invalid token")
	}
	unsigned := parts[0] + "." + parts[1]
	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil || !ed25519.Verify(s.public, []byte(unsigned), signature) {
		return Claims{}, errors.New("invalid signature")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return Claims{}, errors.New("invalid payload")
	}
	var claims Claims
	if err := json.Unmarshal(payload, &claims); err != nil {
		return Claims{}, errors.New("invalid claims")
	}
	if claims.Scope != scope || claims.Subject == uuid.Nil || claims.ExpiresAt <= now.Unix() {
		return Claims{}, errors.New("expired or invalid claims")
	}
	if claims.IssuedAt > now.Add(2*time.Minute).Unix() {
		return Claims{}, errors.New("token issued in the future")
	}
	return claims, nil
}

func RandomToken(bytes int) (string, error) {
	buffer := make([]byte, bytes)
	if _, err := rand.Read(buffer); err != nil {
		return "", fmt.Errorf("random token: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(buffer), nil
}

func HashToken(token string) string {
	digest := sha256.Sum256([]byte(token))
	return hex.EncodeToString(digest[:])
}
