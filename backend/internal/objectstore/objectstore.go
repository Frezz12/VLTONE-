package objectstore

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"
)

var (
	ErrInvalidConfiguration = errors.New("object store configuration is invalid")
	ErrInvalidObject        = errors.New("object does not match the expected content")
	ErrObjectNotFound       = errors.New("object was not found")
	ErrMultipartNotFound    = errors.New("multipart upload was not found")
	ErrInvalidMultipart     = errors.New("multipart upload state is invalid")
	ErrProvider             = errors.New("object store provider request failed")
)

// ExpectedObject is the immutable identity that the collaboration service
// accepts. SHA256 is lowercase hexadecimal and Bytes is always strictly
// positive. Callers must impose a product limit before invoking the store.
type ExpectedObject struct {
	SHA256      string
	Bytes       int64
	ContentType string
}

// PresignedRequest is deliberately limited to the request details a desktop
// client needs. Secret keys and provider responses never cross this boundary.
type PresignedRequest struct {
	Method    string            `json:"method"`
	URL       string            `json:"url"`
	Headers   map[string]string `json:"headers"`
	ExpiresAt time.Time         `json:"expiresAt"`
}

// UploadedPart is provider-observed multipart state. ETag is an opaque entity
// tag and must never be interpreted as a checksum. Bytes is authoritative only
// for validating the multipart layout; VerifyAndPromote still hashes the fully
// assembled object independently.
type UploadedPart struct {
	PartNumber int
	ETag       string
	Bytes      int64
}

// Store abstracts the provider-specific data plane. Every Presign method is a
// local, deterministic signing operation: implementations must not perform
// network I/O or mutate provider state from those methods. Collaboration code
// deliberately invokes them while holding short authorization locks so a
// revoked actor cannot receive a freshly delegated request.
//
// VerifyAndPromote must read and hash the staging object itself (or use an
// equally strong provider-side verified checksum), then create an immutable
// final object under finalKey. Merely trusting client-provided metadata is not
// a valid implementation.
type Store interface {
	PresignPut(ctx context.Context, key string, expected ExpectedObject, ttl time.Duration) (PresignedRequest, error)
	PresignGet(ctx context.Context, key string, ttl time.Duration) (PresignedRequest, error)
	CreateMultipart(ctx context.Context, key string, expected ExpectedObject) (string, error)
	PresignMultipartPart(ctx context.Context, key, providerUploadID string,
		partNumber int, expectedBytes int64, ttl time.Duration) (PresignedRequest, error)
	ListMultipartParts(ctx context.Context, key, providerUploadID string,
		maximumParts int) ([]UploadedPart, error)
	CompleteMultipart(ctx context.Context, key, providerUploadID string,
		parts []UploadedPart) error
	AbortMultipart(ctx context.Context, key, providerUploadID string) error
	VerifyAndPromote(ctx context.Context, stagingKey, finalKey string, expected ExpectedObject) error
	Delete(ctx context.Context, key string) error
}

type ProviderError struct {
	Operation string
	Status    int
}

func (e *ProviderError) Error() string {
	if e.Status == 0 {
		return fmt.Sprintf("object store %s failed", e.Operation)
	}
	return fmt.Sprintf("object store %s failed with status %d", e.Operation, e.Status)
}

func (e *ProviderError) Unwrap() error { return ErrProvider }

func copyExactly(destination io.Writer, source io.Reader, expectedBytes int64) (int64, error) {
	if expectedBytes < 0 {
		return 0, ErrInvalidObject
	}
	written, err := io.Copy(destination, io.LimitReader(source, expectedBytes+1))
	if err != nil {
		return written, err
	}
	if written != expectedBytes {
		return written, ErrInvalidObject
	}
	return written, nil
}
