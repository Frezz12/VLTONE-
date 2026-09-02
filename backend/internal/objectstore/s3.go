package objectstore

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"hash"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	serviceName        = "s3"
	algorithm          = "AWS4-HMAC-SHA256"
	emptyPayloadSHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	maximumPresignTTL  = 7 * 24 * time.Hour
)

type S3Config struct {
	Endpoint        string
	Region          string
	Bucket          string
	AccessKeyID     string
	SecretAccessKey string
	SessionToken    string
	TempDirectory   string
	MaximumBytes    int64
	HTTPClient      *http.Client
	Now             func() time.Time
}

type S3 struct {
	endpoint        *url.URL
	region          string
	bucket          string
	accessKeyID     string
	secretAccessKey string
	sessionToken    string
	tempDirectory   string
	maximumBytes    int64
	client          *http.Client
	now             func() time.Time
}

func NewS3(configuration S3Config) (*S3, error) {
	endpoint, err := url.Parse(strings.TrimSpace(configuration.Endpoint))
	if err != nil || endpoint.Scheme == "" || endpoint.Host == "" || endpoint.User != nil ||
		endpoint.RawQuery != "" || endpoint.Fragment != "" ||
		(endpoint.Path != "" && endpoint.Path != "/") {
		return nil, fmt.Errorf("%w: endpoint must be an absolute HTTP(S) URL without credentials, query or fragment", ErrInvalidConfiguration)
	}
	if endpoint.Scheme != "https" && endpoint.Scheme != "http" {
		return nil, fmt.Errorf("%w: endpoint scheme is unsupported", ErrInvalidConfiguration)
	}
	endpoint.Path = ""
	endpoint.RawPath = ""
	region := strings.TrimSpace(configuration.Region)
	bucket := strings.TrimSpace(configuration.Bucket)
	accessKeyID := strings.TrimSpace(configuration.AccessKeyID)
	if region == "" || !validBucket(bucket) || accessKeyID == "" || configuration.SecretAccessKey == "" {
		return nil, fmt.Errorf("%w: region, DNS-compatible bucket and access keys are required", ErrInvalidConfiguration)
	}
	if strings.ContainsAny(region, "\r\n/") || strings.ContainsAny(accessKeyID, "\r\n") ||
		strings.ContainsAny(configuration.SecretAccessKey, "\r\n") ||
		strings.ContainsAny(configuration.SessionToken, "\r\n") {
		return nil, fmt.Errorf("%w: credential or region contains invalid characters", ErrInvalidConfiguration)
	}
	if configuration.MaximumBytes <= 0 || configuration.MaximumBytes > MaximumObjectBytes {
		return nil, fmt.Errorf("%w: maximum object size is outside S3 limits", ErrInvalidConfiguration)
	}
	if strings.TrimSpace(configuration.TempDirectory) == "" {
		return nil, fmt.Errorf("%w: temporary directory is required", ErrInvalidConfiguration)
	}
	client := configuration.HTTPClient
	if client == nil {
		client = &http.Client{}
	} else {
		// Never mutate a shared caller-owned client.
		copy := *client
		client = &copy
	}
	if client.Timeout <= 0 || client.Timeout > 30*time.Minute {
		client.Timeout = 30 * time.Minute
	}
	// Provider redirects are not part of the signed S3 data plane. Refusing
	// them also guarantees that an Authorization signature cannot be replayed
	// to a redirect target by a custom/injected HTTP client.
	client.CheckRedirect = func(_ *http.Request, _ []*http.Request) error {
		return http.ErrUseLastResponse
	}
	now := configuration.Now
	if now == nil {
		now = func() time.Time { return time.Now().UTC() }
	}
	return &S3{
		endpoint: endpoint, region: region, bucket: bucket,
		accessKeyID: accessKeyID, secretAccessKey: configuration.SecretAccessKey,
		sessionToken: configuration.SessionToken, tempDirectory: configuration.TempDirectory,
		maximumBytes: configuration.MaximumBytes, client: client, now: now,
	}, nil
}

func (s *S3) PresignPut(_ context.Context, key string, expected ExpectedObject, ttl time.Duration) (PresignedRequest, error) {
	if err := s.validateExpected(key, expected); err != nil {
		return PresignedRequest{}, err
	}
	if expected.Bytes > MaximumSinglePutBytes {
		return PresignedRequest{}, fmt.Errorf("%w: object requires multipart upload", ErrInvalidConfiguration)
	}
	headers := map[string]string{
		"content-length":       strconv.FormatInt(expected.Bytes, 10),
		"content-type":         expected.ContentType,
		"x-amz-content-sha256": expected.SHA256,
	}
	return s.presign(http.MethodPut, key, headers, expected.SHA256, ttl)
}

func (s *S3) PresignGet(_ context.Context, key string, ttl time.Duration) (PresignedRequest, error) {
	if !validObjectKey(key) {
		return PresignedRequest{}, fmt.Errorf("%w: invalid object key", ErrInvalidConfiguration)
	}
	return s.presign(http.MethodGet, key, nil, "UNSIGNED-PAYLOAD", ttl)
}

func (s *S3) VerifyAndPromote(ctx context.Context, stagingKey, finalKey string, expected ExpectedObject) error {
	if err := s.validateExpected(stagingKey, expected); err != nil {
		return err
	}
	if !validObjectKey(finalKey) {
		return fmt.Errorf("%w: invalid final object key", ErrInvalidConfiguration)
	}
	temporary, err := os.CreateTemp(s.tempDirectory, ".collab-object-*")
	if err != nil {
		return fmt.Errorf("create verification file: %w", err)
	}
	temporaryName := temporary.Name()
	defer os.Remove(temporaryName)

	if err := s.downloadAndVerify(ctx, stagingKey, temporary, expected); err != nil {
		_ = temporary.Close()
		return err
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return fmt.Errorf("sync verification file: %w", err)
	}
	if _, err := temporary.Seek(0, io.SeekStart); err != nil {
		_ = temporary.Close()
		return fmt.Errorf("rewind verification file: %w", err)
	}
	if err := s.putVerified(ctx, finalKey, temporary, expected); err != nil {
		_ = temporary.Close()
		return err
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close verification file: %w", err)
	}

	// Re-read the server-controlled final key. This protects deployments using
	// S3-compatible providers whose PUT checksum semantics are weaker than AWS.
	return s.verifyRemote(ctx, finalKey, expected)
}

func (s *S3) Delete(ctx context.Context, key string) error {
	if !validObjectKey(key) {
		return fmt.Errorf("%w: invalid object key", ErrInvalidConfiguration)
	}
	request, err := s.signedRequest(ctx, http.MethodDelete, key, nil, 0, "", emptyPayloadSHA256)
	if err != nil {
		return err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return &ProviderError{Operation: "delete"}
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	if response.StatusCode == http.StatusNotFound || response.StatusCode/100 == 2 {
		return nil
	}
	return &ProviderError{Operation: "delete", Status: response.StatusCode}
}

func (s *S3) validateExpected(key string, expected ExpectedObject) error {
	if !validObjectKey(key) || !lowerHexSHA256(expected.SHA256) || expected.Bytes <= 0 ||
		expected.Bytes > s.maximumBytes || !validContentType(expected.ContentType) {
		return fmt.Errorf("%w: invalid expected object", ErrInvalidConfiguration)
	}
	return nil
}

func (s *S3) downloadAndVerify(ctx context.Context, key string, destination io.Writer, expected ExpectedObject) error {
	request, err := s.signedRequest(ctx, http.MethodGet, key, nil, 0, "", emptyPayloadSHA256)
	if err != nil {
		return err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return &ProviderError{Operation: "get staging"}
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNotFound {
		return ErrObjectNotFound
	}
	if response.StatusCode/100 != 2 {
		_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
		return &ProviderError{Operation: "get staging", Status: response.StatusCode}
	}
	if response.ContentLength >= 0 && response.ContentLength != expected.Bytes {
		return ErrInvalidObject
	}
	hasher := sha256.New()
	if _, err := copyExactly(io.MultiWriter(destination, hasher), response.Body, expected.Bytes); err != nil {
		return err
	}
	if hex.EncodeToString(hasher.Sum(nil)) != expected.SHA256 {
		return ErrInvalidObject
	}
	return nil
}

func (s *S3) verifyRemote(ctx context.Context, key string, expected ExpectedObject) error {
	return s.downloadAndVerify(ctx, key, io.Discard, expected)
}

func (s *S3) putVerified(ctx context.Context, key string, body *os.File, expected ExpectedObject) error {
	if expected.Bytes > MaximumSinglePutBytes {
		return s.putVerifiedMultipart(ctx, key, body, expected)
	}
	// LimitReader intentionally hides an underlying io.Closer (notably
	// *os.File) from net/http. The transport closes its request body, while the
	// verification owner retains and closes the temporary file explicitly.
	request, err := s.signedRequest(ctx, http.MethodPut, key,
		io.LimitReader(body, expected.Bytes), expected.Bytes,
		expected.ContentType, expected.SHA256)
	if err != nil {
		return err
	}
	response, err := s.client.Do(request)
	if err != nil {
		return &ProviderError{Operation: "put verified"}
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	if response.StatusCode/100 != 2 {
		return &ProviderError{Operation: "put verified", Status: response.StatusCode}
	}
	return nil
}

func (s *S3) presign(method, key string, headers map[string]string, payloadHash string, ttl time.Duration) (PresignedRequest, error) {
	return s.presignQuery(method, key, nil, headers, payloadHash, ttl)
}

func (s *S3) presignQuery(method, key string, operationQuery,
	headers map[string]string, payloadHash string, ttl time.Duration) (PresignedRequest, error) {
	if ttl <= 0 || ttl > maximumPresignTTL || ttl%time.Second != 0 {
		return PresignedRequest{}, fmt.Errorf("%w: presign expiry is outside the supported range", ErrInvalidConfiguration)
	}
	if !validObjectKey(key) {
		return PresignedRequest{}, fmt.Errorf("%w: invalid object key", ErrInvalidConfiguration)
	}
	now := s.now().UTC()
	target := s.objectURL(key)
	canonicalHeaders, signedHeaders := canonicalizeHeaders(target.Host, headers)
	date := now.Format("20060102")
	timestamp := now.Format("20060102T150405Z")
	scope := date + "/" + s.region + "/" + serviceName + "/aws4_request"
	query := make(map[string]string, len(operationQuery)+7)
	for name, value := range operationQuery {
		query[name] = value
	}
	query["X-Amz-Algorithm"] = algorithm
	query["X-Amz-Credential"] = s.accessKeyID + "/" + scope
	query["X-Amz-Date"] = timestamp
	query["X-Amz-Expires"] = strconv.FormatInt(int64(ttl/time.Second), 10)
	query["X-Amz-SignedHeaders"] = signedHeaders
	if s.sessionToken != "" {
		query["X-Amz-Security-Token"] = s.sessionToken
	}
	canonicalQuery := encodeQuery(query)
	canonicalRequest := strings.Join([]string{
		method, target.EscapedPath(), canonicalQuery, canonicalHeaders, signedHeaders, payloadHash,
	}, "\n")
	stringToSign := stringToSign(timestamp, scope, canonicalRequest)
	signature := hex.EncodeToString(hmacBytes(signingKey(s.secretAccessKey, date, s.region), stringToSign))
	query["X-Amz-Signature"] = signature
	target.RawQuery = encodeQuery(query)

	responseHeaders := make(map[string]string, len(headers))
	for name, value := range headers {
		responseHeaders[http.CanonicalHeaderKey(name)] = value
	}
	return PresignedRequest{
		Method: method, URL: target.String(), Headers: responseHeaders, ExpiresAt: now.Add(ttl),
	}, nil
}

func (s *S3) signedRequest(ctx context.Context, method, key string, body io.Reader,
	contentLength int64, contentType, payloadHash string) (*http.Request, error) {
	return s.signedRequestQuery(ctx, method, key, nil, body,
		contentLength, contentType, payloadHash)
}

func (s *S3) signedRequestQuery(ctx context.Context, method, key string,
	operationQuery map[string]string, body io.Reader,
	contentLength int64, contentType, payloadHash string) (*http.Request, error) {
	if !validObjectKey(key) {
		return nil, fmt.Errorf("%w: invalid object key", ErrInvalidConfiguration)
	}
	now := s.now().UTC()
	target := s.objectURL(key)
	if len(operationQuery) != 0 {
		target.RawQuery = encodeQuery(operationQuery)
	}
	request, err := http.NewRequestWithContext(ctx, method, target.String(), body)
	if err != nil {
		return nil, err
	}
	if contentLength > 0 {
		request.ContentLength = contentLength
		request.Header.Set("Content-Length", strconv.FormatInt(contentLength, 10))
	}
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	request.Header.Set("X-Amz-Content-Sha256", payloadHash)
	request.Header.Set("X-Amz-Date", now.Format("20060102T150405Z"))
	if method == http.MethodGet {
		// Hash the stored representation exactly. net/http otherwise negotiates
		// gzip and may transparently decode a provider object's Content-Encoding.
		request.Header.Set("Accept-Encoding", "identity")
	}
	if s.sessionToken != "" {
		request.Header.Set("X-Amz-Security-Token", s.sessionToken)
	}

	headers := make(map[string]string, len(request.Header))
	for name, values := range request.Header {
		headers[name] = strings.Join(values, ",")
	}
	canonicalHeaders, signedHeaders := canonicalizeHeaders(target.Host, headers)
	canonicalRequest := strings.Join([]string{
		method, target.EscapedPath(), target.RawQuery, canonicalHeaders, signedHeaders, payloadHash,
	}, "\n")
	date := now.Format("20060102")
	scope := date + "/" + s.region + "/" + serviceName + "/aws4_request"
	signature := hex.EncodeToString(hmacBytes(signingKey(s.secretAccessKey, date, s.region),
		stringToSign(now.Format("20060102T150405Z"), scope, canonicalRequest)))
	request.Header.Set("Authorization", algorithm+" Credential="+s.accessKeyID+"/"+scope+
		", SignedHeaders="+signedHeaders+", Signature="+signature)
	return request, nil
}

func (s *S3) objectURL(key string) *url.URL {
	target := *s.endpoint
	segments := strings.Split(key, "/")
	escaped := make([]string, 0, len(segments)+1)
	escaped = append(escaped, awsEscape(s.bucket))
	for _, segment := range segments {
		escaped = append(escaped, awsEscape(segment))
	}
	base := strings.TrimRight(target.EscapedPath(), "/")
	target.RawPath = base + "/" + strings.Join(escaped, "/")
	target.Path, _ = url.PathUnescape(target.RawPath)
	return &target
}

func canonicalizeHeaders(host string, headers map[string]string) (string, string) {
	values := make(map[string]string, len(headers)+1)
	values["host"] = strings.TrimSpace(host)
	for name, value := range headers {
		name = strings.ToLower(strings.TrimSpace(name))
		if name == "authorization" || name == "host" {
			continue
		}
		values[name] = strings.Join(strings.Fields(value), " ")
	}
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	var canonical strings.Builder
	for _, name := range names {
		canonical.WriteString(name)
		canonical.WriteByte(':')
		canonical.WriteString(values[name])
		canonical.WriteByte('\n')
	}
	return canonical.String(), strings.Join(names, ";")
}

func stringToSign(timestamp, scope, canonicalRequest string) string {
	digest := sha256.Sum256([]byte(canonicalRequest))
	return algorithm + "\n" + timestamp + "\n" + scope + "\n" + hex.EncodeToString(digest[:])
}

func signingKey(secret, date, region string) []byte {
	dateKey := hmacBytes([]byte("AWS4"+secret), date)
	regionKey := hmacBytes(dateKey, region)
	serviceKey := hmacBytes(regionKey, serviceName)
	return hmacBytes(serviceKey, "aws4_request")
}

func hmacBytes(key []byte, value string) []byte {
	var result hash.Hash = hmac.New(sha256.New, key)
	_, _ = result.Write([]byte(value))
	return result.Sum(nil)
}

func encodeQuery(values map[string]string) string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, key := range keys {
		parts = append(parts, awsEscape(key)+"="+awsEscape(values[key]))
	}
	return strings.Join(parts, "&")
}

func awsEscape(value string) string {
	escaped := url.QueryEscape(value)
	escaped = strings.ReplaceAll(escaped, "+", "%20")
	escaped = strings.ReplaceAll(escaped, "%7E", "~")
	return escaped
}

func validObjectKey(value string) bool {
	if value == "" || len(value) > 1024 || strings.HasPrefix(value, "/") || strings.HasSuffix(value, "/") {
		return false
	}
	cleaned := path.Clean(value)
	if cleaned != value || strings.Contains(value, "//") {
		return false
	}
	for _, segment := range strings.Split(value, "/") {
		if segment == "." || segment == ".." || segment == "" {
			return false
		}
	}
	for _, character := range value {
		if (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			character == '/' || character == '-' || character == '_' || character == '.' {
			continue
		}
		return false
	}
	return true
}

func validBucket(value string) bool {
	if len(value) < 3 || len(value) > 63 || strings.HasPrefix(value, ".") || strings.HasSuffix(value, ".") ||
		strings.HasPrefix(value, "-") || strings.HasSuffix(value, "-") || strings.Contains(value, "..") {
		return false
	}
	for _, character := range value {
		if (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			character == '.' || character == '-' {
			continue
		}
		return false
	}
	return true
}

func lowerHexSHA256(value string) bool {
	if len(value) != sha256.Size*2 || strings.ToLower(value) != value {
		return false
	}
	decoded, err := hex.DecodeString(value)
	return err == nil && len(decoded) == sha256.Size
}

func validContentType(value string) bool {
	value = strings.TrimSpace(value)
	return value != "" && len(value) <= 160 && !strings.ContainsAny(value, "\r\n")
}

func IsInvalidObject(err error) bool {
	return errors.Is(err, ErrInvalidObject) || errors.Is(err, ErrObjectNotFound)
}
