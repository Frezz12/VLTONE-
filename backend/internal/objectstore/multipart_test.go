package objectstore

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestS3MultipartLifecycleUsesProviderObservedParts(t *testing.T) {
	const providerUploadID = "opaque/provider+upload=id"
	var mutex sync.Mutex
	completedBody := ""
	aborted := false
	truncated := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" {
			t.Error("server-side multipart request was not signed")
		}
		query := r.URL.Query()
		_, createsUpload := query["uploads"]
		switch {
		case r.Method == http.MethodPost && createsUpload:
			w.Header().Set("Content-Type", "application/xml")
			_, _ = io.WriteString(w, `<InitiateMultipartUploadResult><UploadId>`+
				providerUploadID+`</UploadId></InitiateMultipartUploadResult>`)
		case r.Method == http.MethodGet && query.Get("uploadId") == providerUploadID:
			if r.Header.Get("Accept-Encoding") != "identity" {
				t.Error("ListParts did not request the exact provider representation")
			}
			isTruncated := "false"
			mutex.Lock()
			if truncated {
				isTruncated = "true"
			}
			mutex.Unlock()
			_, _ = io.WriteString(w, `<ListPartsResult><IsTruncated>`+isTruncated+
				`</IsTruncated><Part><PartNumber>1</PartNumber><ETag>&quot;etag-one&quot;</ETag><Size>5242880</Size></Part>`+
				`<Part><PartNumber>2</PartNumber><ETag>&quot;etag-two&quot;</ETag><Size>7</Size></Part></ListPartsResult>`)
		case r.Method == http.MethodPost && query.Get("uploadId") == providerUploadID:
			body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
			if err != nil {
				t.Error(err)
			}
			mutex.Lock()
			completedBody = string(body)
			mutex.Unlock()
			_, _ = io.WriteString(w, `<CompleteMultipartUploadResult><ETag>&quot;final-etag&quot;</ETag></CompleteMultipartUploadResult>`)
		case r.Method == http.MethodDelete && query.Get("uploadId") == providerUploadID:
			mutex.Lock()
			aborted = true
			mutex.Unlock()
			w.WriteHeader(http.StatusNoContent)
		default:
			http.Error(w, "unexpected multipart request", http.StatusBadRequest)
		}
	}))
	defer server.Close()

	store, err := NewS3(S3Config{
		Endpoint: server.URL, Region: "us-east-1", Bucket: "vlt-projects",
		AccessKeyID: "access", SecretAccessKey: "secret", TempDirectory: t.TempDir(),
		MaximumBytes: 32 << 20, HTTPClient: server.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	expected := ExpectedObject{
		SHA256: strings.Repeat("a", 64), Bytes: (5 << 20) + 7, ContentType: "audio/wav",
	}
	uploadID, err := store.CreateMultipart(context.Background(), "uploads/project/upload", expected)
	if err != nil || uploadID != providerUploadID {
		t.Fatalf("multipart create failed: id=%q err=%v", uploadID, err)
	}
	presigned, err := store.PresignMultipartPart(context.Background(), "uploads/project/upload",
		uploadID, 1, 5<<20, 10*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := url.Parse(presigned.URL)
	if err != nil {
		t.Fatal(err)
	}
	if parsed.Query().Get("partNumber") != "1" ||
		parsed.Query().Get("uploadId") != providerUploadID ||
		presigned.Headers["Content-Length"] != "5242880" ||
		parsed.Query().Get("X-Amz-Signature") == "" {
		t.Fatalf("multipart part was not bound correctly: %#v %s", presigned.Headers, parsed.RawQuery)
	}

	parts, err := store.ListMultipartParts(context.Background(), "uploads/project/upload",
		uploadID, MaximumMultipartParts)
	if err != nil || len(parts) != 2 || parts[0].ETag != `"etag-one"` || parts[1].Bytes != 7 {
		t.Fatalf("provider part state was not parsed: parts=%+v err=%v", parts, err)
	}
	if err := store.CompleteMultipart(context.Background(), "uploads/project/upload",
		uploadID, parts); err != nil {
		t.Fatal(err)
	}
	mutex.Lock()
	requestXML := completedBody
	mutex.Unlock()
	if !strings.Contains(requestXML, `<PartNumber>1</PartNumber>`) ||
		!strings.Contains(requestXML, `<ETag>&#34;etag-two&#34;</ETag>`) ||
		strings.Contains(requestXML, providerUploadID) {
		t.Fatalf("unexpected complete multipart XML: %s", requestXML)
	}
	if err := store.AbortMultipart(context.Background(), "uploads/project/upload", uploadID); err != nil {
		t.Fatal(err)
	}
	mutex.Lock()
	wasAborted := aborted
	mutex.Unlock()
	if !wasAborted {
		t.Fatal("provider multipart abort was not sent")
	}

	mutex.Lock()
	truncated = true
	mutex.Unlock()
	if _, err := store.ListMultipartParts(context.Background(), "uploads/project/upload",
		uploadID, MaximumMultipartParts); !errors.Is(err, ErrInvalidMultipart) {
		t.Fatalf("truncated provider state was accepted: %v", err)
	}
}

func TestMultipartBoundsAndOpaqueIdentifiers(t *testing.T) {
	store, err := NewS3(S3Config{
		Endpoint: "https://objects.example.test", Region: "us-east-1",
		Bucket: "vlt-projects", AccessKeyID: "access", SecretAccessKey: "secret",
		TempDirectory: t.TempDir(), MaximumBytes: 64 << 30,
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		name       string
		uploadID   string
		partNumber int
		bytes      int64
	}{
		{"empty provider id", "", 1, MinimumMultipartBytes},
		{"control in provider id", "provider\nleak", 1, MinimumMultipartBytes},
		{"zero part", "provider-id", 0, MinimumMultipartBytes},
		{"too many parts", "provider-id", MaximumMultipartParts + 1, MinimumMultipartBytes},
		{"oversized part", "provider-id", 1, MaximumMultipartBytes + 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, err := store.PresignMultipartPart(context.Background(), "uploads/project/upload",
				test.uploadID, test.partNumber, test.bytes, time.Minute)
			if !errors.Is(err, ErrInvalidConfiguration) {
				t.Fatalf("invalid multipart part was accepted: %v", err)
			}
		})
	}
	if _, err := store.PresignPut(context.Background(), "uploads/project/oversized-single",
		ExpectedObject{
			SHA256: strings.Repeat("a", 64), Bytes: MaximumSinglePutBytes + 1,
			ContentType: "audio/wav",
		}, time.Minute); !errors.Is(err, ErrInvalidConfiguration) {
		t.Fatalf("object above the S3 single-PUT limit was accepted: %v", err)
	}
}

func TestVerifiedMultipartPromotionStreamsBoundedExactParts(t *testing.T) {
	const providerUploadID = "verified-provider-upload"
	data := append(bytes.Repeat([]byte("a"), MinimumMultipartBytes), []byte("tail-007")...)
	digest := sha256.Sum256(data)
	var mutex sync.Mutex
	providerParts := make(map[int][]byte)
	completed := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" {
			t.Error("server-side verified multipart request was not signed")
		}
		query := r.URL.Query()
		_, createsUpload := query["uploads"]
		switch {
		case r.Method == http.MethodPost && createsUpload:
			_, _ = io.WriteString(w, `<InitiateMultipartUploadResult><UploadId>`+
				providerUploadID+`</UploadId></InitiateMultipartUploadResult>`)
		case r.Method == http.MethodPut && query.Get("uploadId") == providerUploadID:
			partNumber, err := strconv.Atoi(query.Get("partNumber"))
			if err != nil {
				t.Error(err)
			}
			body, err := io.ReadAll(io.LimitReader(r.Body, MinimumMultipartBytes+1024))
			if err != nil {
				t.Error(err)
			}
			partDigest := sha256.Sum256(body)
			if got := r.Header.Get("X-Amz-Content-Sha256"); got != hex.EncodeToString(partDigest[:]) {
				t.Errorf("part payload hash was not exact: %q", got)
			}
			mutex.Lock()
			providerParts[partNumber] = body
			mutex.Unlock()
			w.Header().Set("ETag", `"provider-part-`+strconv.Itoa(partNumber)+`"`)
			w.WriteHeader(http.StatusOK)
		case r.Method == http.MethodPost && query.Get("uploadId") == providerUploadID:
			mutex.Lock()
			completed = true
			mutex.Unlock()
			_, _ = io.WriteString(w, `<CompleteMultipartUploadResult><ETag>&quot;final&quot;</ETag></CompleteMultipartUploadResult>`)
		case r.Method == http.MethodDelete && query.Get("uploadId") == providerUploadID:
			w.WriteHeader(http.StatusNoContent)
		default:
			http.Error(w, "unexpected verified multipart request", http.StatusBadRequest)
		}
	}))
	defer server.Close()

	store, err := NewS3(S3Config{
		Endpoint: server.URL, Region: "us-east-1", Bucket: "vlt-projects",
		AccessKeyID: "access", SecretAccessKey: "secret", TempDirectory: t.TempDir(),
		MaximumBytes: int64(len(data)), HTTPClient: server.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	file, err := os.CreateTemp(t.TempDir(), "verified-multipart-*")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if _, err := file.Write(data); err != nil {
		t.Fatal(err)
	}
	expected := ExpectedObject{
		SHA256: hex.EncodeToString(digest[:]), Bytes: int64(len(data)), ContentType: "audio/wav",
	}
	if err := store.putVerifiedMultipartSized(context.Background(), "blobs/aa/verified",
		file, expected, MinimumMultipartBytes); err != nil {
		t.Fatal(err)
	}
	mutex.Lock()
	first, second, wasCompleted := providerParts[1], providerParts[2], completed
	mutex.Unlock()
	if !wasCompleted || len(first) != MinimumMultipartBytes || string(second) != "tail-007" {
		t.Fatalf("verified promotion did not preserve exact bounded parts: first=%d second=%q complete=%t",
			len(first), string(second), wasCompleted)
	}
}

func TestVerifiedMultipartLayoutCoversMaximumConfiguredObject(t *testing.T) {
	partSize, partCount, err := verifiedMultipartLayout(MaximumObjectBytes,
		verifiedMultipartTargetBytes)
	if err != nil {
		t.Fatal(err)
	}
	if partSize != MaximumMultipartBytes || partCount != MaximumMultipartParts {
		t.Fatalf("unexpected maximum verified layout: partSize=%d parts=%d", partSize, partCount)
	}
}
