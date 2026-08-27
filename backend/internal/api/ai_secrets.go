package api

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"io"
)

const aiSecretVersion = "v1:"

func derivedAISecretKey(seed []byte) [32]byte {
	if len(seed) != 32 {
		seed = []byte("VLT Studio development AI secret key")
	}
	material := append([]byte("vlt-ai-model-credentials\x00"), seed...)
	return sha256.Sum256(material)
}

func aiSecretCipher(key [32]byte) (cipher.AEAD, error) {
	block, err := aes.NewCipher(key[:])
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}

func (s *Server) encryptAISecret(secret string) (string, error) {
	if secret == "" {
		return "", errors.New("empty AI secret")
	}
	seed := s.Config.AICredentialsKey
	if len(seed) != 32 {
		seed = s.Config.SigningSeed
	}
	aead, err := aiSecretCipher(derivedAISecretKey(seed))
	if err != nil {
		return "", err
	}
	nonce := make([]byte, aead.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return "", err
	}
	sealed := aead.Seal(nonce, nonce, []byte(secret), []byte(aiSecretVersion))
	return aiSecretVersion + base64.RawStdEncoding.EncodeToString(sealed), nil
}

func (s *Server) decryptAISecret(ciphertext string) (string, error) {
	if len(ciphertext) <= len(aiSecretVersion) || ciphertext[:len(aiSecretVersion)] != aiSecretVersion {
		return "", errors.New("unsupported AI secret format")
	}
	sealed, err := base64.RawStdEncoding.DecodeString(ciphertext[len(aiSecretVersion):])
	if err != nil {
		return "", err
	}
	seeds := [][]byte{s.Config.SigningSeed}
	if len(s.Config.AICredentialsKey) == 32 {
		// The dedicated key is primary. Keeping the signing seed as a fallback
		// lets an existing installation add AI_CREDENTIALS_KEY without having
		// to re-enter every provider credential at once.
		seeds = [][]byte{s.Config.AICredentialsKey, s.Config.SigningSeed}
	}
	var lastErr error
	for _, seed := range seeds {
		aead, cipherErr := aiSecretCipher(derivedAISecretKey(seed))
		if cipherErr != nil {
			lastErr = cipherErr
			continue
		}
		if len(sealed) < aead.NonceSize() {
			return "", errors.New("invalid AI secret")
		}
		nonce, body := sealed[:aead.NonceSize()], sealed[aead.NonceSize():]
		plain, openErr := aead.Open(nil, nonce, body, []byte(aiSecretVersion))
		if openErr == nil {
			return string(plain), nil
		}
		lastErr = openErr
	}
	return "", lastErr
}
