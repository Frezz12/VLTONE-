package objectstore

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"time"
)

const (
	verifiedMultipartTargetBytes = int64(64 << 20)
	verifiedMultipartAlignment   = int64(1 << 20)
	providerCleanupTimeout       = 15 * time.Second
)

// putVerifiedMultipart promotes an already independently verified local file
// without exceeding S3's PutObject limit. Bytes still travel only between the
// backend and the object provider; the collaboration API never proxies them.
func (s *S3) putVerifiedMultipart(ctx context.Context, key string, source io.ReaderAt,
	expected ExpectedObject) error {
	return s.putVerifiedMultipartSized(ctx, key, source, expected,
		verifiedMultipartTargetBytes)
}

func (s *S3) putVerifiedMultipartSized(ctx context.Context, key string,
	source io.ReaderAt, expected ExpectedObject, preferredPartBytes int64) error {
	partSize, partCount, err := verifiedMultipartLayout(expected.Bytes, preferredPartBytes)
	if err != nil {
		return err
	}
	providerUploadID, err := s.CreateMultipart(ctx, key, expected)
	if err != nil {
		return err
	}
	completed := false
	defer func() {
		if completed {
			return
		}
		cleanupContext, cancel := context.WithTimeout(context.Background(), providerCleanupTimeout)
		_ = s.AbortMultipart(cleanupContext, key, providerUploadID)
		cancel()
	}()

	parts := make([]UploadedPart, 0, partCount)
	for partNumber := 1; partNumber <= partCount; partNumber++ {
		offset := int64(partNumber-1) * partSize
		partBytes := partSize
		if partNumber == partCount {
			partBytes = expected.Bytes - offset
		}
		if partBytes <= 0 || partBytes > MaximumMultipartBytes {
			return ErrInvalidMultipart
		}

		// Hash exactly the bytes sent for this part. A second SectionReader keeps
		// memory bounded and prevents a partial transport read from changing the
		// source offset used by another part.
		hasher := sha256.New()
		if _, err := copyExactly(hasher, io.NewSectionReader(source, offset, partBytes),
			partBytes); err != nil {
			return fmt.Errorf("hash verified multipart part: %w", err)
		}
		part, err := s.putVerifiedMultipartPart(ctx, key, providerUploadID,
			partNumber, io.NewSectionReader(source, offset, partBytes), partBytes,
			hex.EncodeToString(hasher.Sum(nil)))
		if err != nil {
			return err
		}
		parts = append(parts, part)
	}
	if err := s.CompleteMultipart(ctx, key, providerUploadID, parts); err != nil {
		return err
	}
	completed = true
	return nil
}

func (s *S3) putVerifiedMultipartPart(ctx context.Context, key, providerUploadID string,
	partNumber int, body io.Reader, expectedBytes int64, payloadSHA256 string) (UploadedPart, error) {
	if !validObjectKey(key) || !validProviderUploadID(providerUploadID) ||
		partNumber < 1 || partNumber > MaximumMultipartParts || expectedBytes <= 0 ||
		expectedBytes > MaximumMultipartBytes || !lowerHexSHA256(payloadSHA256) {
		return UploadedPart{}, fmt.Errorf("%w: invalid verified multipart part", ErrInvalidConfiguration)
	}
	request, err := s.signedRequestQuery(ctx, http.MethodPut, key, map[string]string{
		"partNumber": strconv.Itoa(partNumber), "uploadId": providerUploadID,
	}, body, expectedBytes, "", payloadSHA256)
	if err != nil {
		return UploadedPart{}, err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return UploadedPart{}, &ProviderError{Operation: "put verified multipart part"}
	}
	defer response.Body.Close()
	drainProviderResponse(response.Body)
	if response.StatusCode == http.StatusNotFound {
		return UploadedPart{}, ErrMultipartNotFound
	}
	if response.StatusCode/100 != 2 {
		return UploadedPart{}, &ProviderError{
			Operation: "put verified multipart part", Status: response.StatusCode,
		}
	}
	eTag := response.Header.Get("ETag")
	if !validETag(eTag) {
		return UploadedPart{}, ErrInvalidMultipart
	}
	return UploadedPart{PartNumber: partNumber, ETag: eTag, Bytes: expectedBytes}, nil
}

func verifiedMultipartLayout(totalBytes, preferredPartBytes int64) (int64, int, error) {
	if totalBytes <= 0 || totalBytes > MaximumObjectBytes || preferredPartBytes <= 0 {
		return 0, 0, fmt.Errorf("%w: verified multipart size is invalid", ErrInvalidConfiguration)
	}
	partSize := preferredPartBytes
	if partSize < MinimumMultipartBytes {
		partSize = MinimumMultipartBytes
	}
	requiredForCap := divideRoundUpInt64(totalBytes, MaximumMultipartParts)
	if requiredForCap > partSize {
		partSize = requiredForCap
	}
	partSize = alignRoundUpInt64(partSize, verifiedMultipartAlignment)
	if partSize < MinimumMultipartBytes || partSize > MaximumMultipartBytes ||
		partSize >= totalBytes {
		return 0, 0, fmt.Errorf("%w: verified multipart layout is invalid", ErrInvalidConfiguration)
	}
	partCount64 := divideRoundUpInt64(totalBytes, partSize)
	if partCount64 < 2 || partCount64 > MaximumMultipartParts {
		return 0, 0, fmt.Errorf("%w: verified multipart part count is invalid", ErrInvalidConfiguration)
	}
	return partSize, int(partCount64), nil
}

func divideRoundUpInt64(value, divisor int64) int64 {
	return ((value - 1) / divisor) + 1
}

func alignRoundUpInt64(value, alignment int64) int64 {
	return ((value-1)/alignment + 1) * alignment
}
