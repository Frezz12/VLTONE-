package objectstore

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const (
	// VLT deliberately stays below S3's 10,000-part provider maximum. This
	// bounds ListParts XML, prepare responses and completion request parsing.
	MaximumMultipartParts = 1000
	MinimumMultipartBytes = 5 << 20
	MaximumMultipartBytes = 5 << 30
	// S3 PutObject is limited to 5 GiB. Larger verified objects are promoted
	// with a separate server-originated multipart upload.
	MaximumSinglePutBytes = 5 << 30
	MaximumObjectBytes    = MaximumMultipartBytes * MaximumMultipartParts

	maximumInitiateResponseBytes = 64 << 10
	maximumListResponseBytes     = 2 << 20
	maximumCompleteResponseBytes = 1 << 20
	maximumProviderUploadIDBytes = 2048
	maximumETagBytes             = 256
)

type initiateMultipartResult struct {
	XMLName  xml.Name `xml:"InitiateMultipartUploadResult"`
	UploadID string   `xml:"UploadId"`
}

type listMultipartResult struct {
	XMLName     xml.Name `xml:"ListPartsResult"`
	IsTruncated bool     `xml:"IsTruncated"`
	Parts       []struct {
		PartNumber int    `xml:"PartNumber"`
		ETag       string `xml:"ETag"`
		Bytes      int64  `xml:"Size"`
	} `xml:"Part"`
}

type completeMultipartRequest struct {
	XMLName xml.Name                       `xml:"CompleteMultipartUpload"`
	Parts   []completeMultipartRequestPart `xml:"Part"`
}

type completeMultipartRequestPart struct {
	PartNumber int    `xml:"PartNumber"`
	ETag       string `xml:"ETag"`
}

type completeMultipartResult struct {
	XMLName xml.Name `xml:"CompleteMultipartUploadResult"`
	ETag    string   `xml:"ETag"`
}

type providerErrorEnvelope struct {
	XMLName xml.Name `xml:"Error"`
	Code    string   `xml:"Code"`
}

func (s *S3) CreateMultipart(ctx context.Context, key string,
	expected ExpectedObject) (string, error) {
	if err := s.validateExpected(key, expected); err != nil {
		return "", err
	}
	request, err := s.signedRequestQuery(ctx, http.MethodPost, key,
		map[string]string{"uploads": ""}, nil, 0,
		expected.ContentType, emptyPayloadSHA256)
	if err != nil {
		return "", err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return "", &ProviderError{Operation: "create multipart"}
	}
	defer response.Body.Close()
	if response.StatusCode/100 != 2 {
		drainProviderResponse(response.Body)
		return "", &ProviderError{Operation: "create multipart", Status: response.StatusCode}
	}
	body, err := readProviderResponse(response.Body, maximumInitiateResponseBytes)
	if err != nil {
		return "", err
	}
	var result initiateMultipartResult
	if err := xml.Unmarshal(body, &result); err != nil ||
		result.XMLName.Local != "InitiateMultipartUploadResult" ||
		!validProviderUploadID(result.UploadID) {
		return "", ErrInvalidMultipart
	}
	return result.UploadID, nil
}

func (s *S3) PresignMultipartPart(_ context.Context, key, providerUploadID string,
	partNumber int, expectedBytes int64, ttl time.Duration) (PresignedRequest, error) {
	if !validObjectKey(key) || !validProviderUploadID(providerUploadID) ||
		partNumber < 1 || partNumber > MaximumMultipartParts ||
		expectedBytes <= 0 || expectedBytes > MaximumMultipartBytes {
		return PresignedRequest{}, fmt.Errorf("%w: invalid multipart part", ErrInvalidConfiguration)
	}
	headers := map[string]string{
		"content-length": strconv.FormatInt(expectedBytes, 10),
	}
	return s.presignQuery(http.MethodPut, key, map[string]string{
		"partNumber": strconv.Itoa(partNumber), "uploadId": providerUploadID,
	}, headers, "UNSIGNED-PAYLOAD", ttl)
}

func (s *S3) ListMultipartParts(ctx context.Context, key, providerUploadID string,
	maximumParts int) ([]UploadedPart, error) {
	if !validObjectKey(key) || !validProviderUploadID(providerUploadID) ||
		maximumParts < 1 || maximumParts > MaximumMultipartParts {
		return nil, fmt.Errorf("%w: invalid multipart list request", ErrInvalidConfiguration)
	}
	request, err := s.signedRequestQuery(ctx, http.MethodGet, key, map[string]string{
		"uploadId": providerUploadID, "max-parts": strconv.Itoa(maximumParts),
	}, nil, 0, "", emptyPayloadSHA256)
	if err != nil {
		return nil, err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return nil, &ProviderError{Operation: "list multipart parts"}
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNotFound {
		drainProviderResponse(response.Body)
		return nil, ErrMultipartNotFound
	}
	if response.StatusCode/100 != 2 {
		drainProviderResponse(response.Body)
		return nil, &ProviderError{Operation: "list multipart parts", Status: response.StatusCode}
	}
	body, err := readProviderResponse(response.Body, maximumListResponseBytes)
	if err != nil {
		return nil, err
	}
	var result listMultipartResult
	if err := xml.Unmarshal(body, &result); err != nil ||
		result.XMLName.Local != "ListPartsResult" || result.IsTruncated ||
		len(result.Parts) > maximumParts {
		return nil, ErrInvalidMultipart
	}
	parts := make([]UploadedPart, 0, len(result.Parts))
	previous := 0
	for _, part := range result.Parts {
		if part.PartNumber <= previous || part.PartNumber > MaximumMultipartParts ||
			part.Bytes <= 0 || part.Bytes > MaximumMultipartBytes || !validETag(part.ETag) {
			return nil, ErrInvalidMultipart
		}
		parts = append(parts, UploadedPart{
			PartNumber: part.PartNumber, ETag: part.ETag, Bytes: part.Bytes,
		})
		previous = part.PartNumber
	}
	return parts, nil
}

func (s *S3) CompleteMultipart(ctx context.Context, key, providerUploadID string,
	parts []UploadedPart) error {
	if !validObjectKey(key) || !validProviderUploadID(providerUploadID) ||
		len(parts) < 1 || len(parts) > MaximumMultipartParts {
		return fmt.Errorf("%w: invalid multipart completion", ErrInvalidConfiguration)
	}
	requestBody := completeMultipartRequest{
		Parts: make([]completeMultipartRequestPart, 0, len(parts)),
	}
	previous := 0
	for _, part := range parts {
		if part.PartNumber <= previous || part.PartNumber > MaximumMultipartParts ||
			!validETag(part.ETag) {
			return ErrInvalidMultipart
		}
		requestBody.Parts = append(requestBody.Parts, completeMultipartRequestPart{
			PartNumber: part.PartNumber, ETag: part.ETag,
		})
		previous = part.PartNumber
	}
	body, err := xml.Marshal(requestBody)
	if err != nil {
		return ErrInvalidMultipart
	}
	digest := sha256.Sum256(body)
	request, err := s.signedRequestQuery(ctx, http.MethodPost, key,
		map[string]string{"uploadId": providerUploadID}, bytes.NewReader(body), int64(len(body)),
		"application/xml", hex.EncodeToString(digest[:]))
	if err != nil {
		return err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return &ProviderError{Operation: "complete multipart"}
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNotFound {
		drainProviderResponse(response.Body)
		return ErrMultipartNotFound
	}
	if response.StatusCode/100 != 2 {
		drainProviderResponse(response.Body)
		return &ProviderError{Operation: "complete multipart", Status: response.StatusCode}
	}
	responseBody, err := readProviderResponse(response.Body, maximumCompleteResponseBytes)
	if err != nil {
		return err
	}
	var providerFailure providerErrorEnvelope
	if err := xml.Unmarshal(responseBody, &providerFailure); err == nil &&
		providerFailure.XMLName.Local == "Error" {
		if strings.TrimSpace(providerFailure.Code) == "NoSuchUpload" {
			return ErrMultipartNotFound
		}
		return &ProviderError{Operation: "complete multipart"}
	}
	var result completeMultipartResult
	if err := xml.Unmarshal(responseBody, &result); err != nil ||
		result.XMLName.Local != "CompleteMultipartUploadResult" || !validETag(result.ETag) {
		return ErrInvalidMultipart
	}
	return nil
}

func (s *S3) AbortMultipart(ctx context.Context, key, providerUploadID string) error {
	if !validObjectKey(key) || !validProviderUploadID(providerUploadID) {
		return fmt.Errorf("%w: invalid multipart abort", ErrInvalidConfiguration)
	}
	request, err := s.signedRequestQuery(ctx, http.MethodDelete, key,
		map[string]string{"uploadId": providerUploadID}, nil, 0, "", emptyPayloadSHA256)
	if err != nil {
		return err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return &ProviderError{Operation: "abort multipart"}
	}
	defer response.Body.Close()
	drainProviderResponse(response.Body)
	if response.StatusCode == http.StatusNotFound || response.StatusCode/100 == 2 {
		return nil
	}
	return &ProviderError{Operation: "abort multipart", Status: response.StatusCode}
}

func readProviderResponse(source io.Reader, maximum int64) ([]byte, error) {
	body, err := io.ReadAll(io.LimitReader(source, maximum+1))
	if err != nil {
		return nil, &ProviderError{Operation: "read provider response"}
	}
	if int64(len(body)) > maximum {
		return nil, ErrInvalidMultipart
	}
	return body, nil
}

func drainProviderResponse(source io.Reader) {
	_, _ = io.Copy(io.Discard, io.LimitReader(source, 4096))
}

func validProviderUploadID(value string) bool {
	if value == "" || len(value) > maximumProviderUploadIDBytes ||
		!utf8.ValidString(value) || strings.TrimSpace(value) != value {
		return false
	}
	for _, character := range value {
		if character < 0x20 || character == 0x7f {
			return false
		}
	}
	return true
}

func validETag(value string) bool {
	if value == "" || len(value) > maximumETagBytes ||
		!utf8.ValidString(value) || strings.TrimSpace(value) != value {
		return false
	}
	for _, character := range value {
		if character < 0x20 || character == 0x7f {
			return false
		}
	}
	return true
}
