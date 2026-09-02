package objectstore

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"
)

const testSHA256 = "8ed3f6ad685b959ead7022518e1af76cd816f8e8ec7ccdda1ed4018e8f2223f8"

func TestPresignedPutIsDeterministicAndBindsContent(t *testing.T) {
	store, err := NewS3(S3Config{
		Endpoint: "https://objects.example.test", Region: "eu-test-1",
		Bucket: "vlt-projects", AccessKeyID: "AKIDEXAMPLE",
		SecretAccessKey: "test-secret-key", TempDirectory: t.TempDir(),
		MaximumBytes: 1024, Now: func() time.Time {
			return time.Date(2026, 8, 29, 12, 34, 56, 0, time.UTC)
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	request, err := store.PresignPut(context.Background(),
		"uploads/11111111-1111-4111-8111-111111111111/22222222-2222-4222-8222-222222222222",
		ExpectedObject{SHA256: testSHA256, Bytes: 5, ContentType: "audio/wav"}, 15*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := url.Parse(request.URL)
	if err != nil {
		t.Fatal(err)
	}
	if parsed.Query().Get("X-Amz-Algorithm") != algorithm ||
		parsed.Query().Get("X-Amz-Credential") != "AKIDEXAMPLE/20260829/eu-test-1/s3/aws4_request" ||
		parsed.Query().Get("X-Amz-Expires") != "900" ||
		parsed.Query().Get("X-Amz-Signature") != "2d607f8ab505d132cd8651d8ba684eca2816d338f630f91bd4e68fdcee31a92f" {
		t.Fatalf("unexpected SigV4 query: %s", parsed.RawQuery)
	}
	if request.Headers["Content-Length"] != "5" ||
		request.Headers["Content-Type"] != "audio/wav" ||
		request.Headers["X-Amz-Content-Sha256"] != testSHA256 {
		t.Fatalf("presigned PUT did not bind exact bytes: %#v", request.Headers)
	}
	if strings.Contains(request.URL, "test-secret-key") {
		t.Fatal("secret access key leaked into presigned URL")
	}
}

func TestVerifyAndPromoteStreamsAndRechecksFinalObject(t *testing.T) {
	var mutex sync.Mutex
	objects := map[string][]byte{
		"/vlt-projects/uploads/project/upload": []byte("alpha"),
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" || strings.Contains(r.Header.Get("Authorization"), "test-secret-key") {
			t.Errorf("request authorization is missing or leaked a secret")
		}
		if r.Method == http.MethodGet && r.Header.Get("Accept-Encoding") != "identity" {
			t.Errorf("verification GET did not request the exact stored representation")
		}
		mutex.Lock()
		defer mutex.Unlock()
		switch r.Method {
		case http.MethodGet:
			value, ok := objects[r.URL.Path]
			if !ok {
				http.NotFound(w, r)
				return
			}
			_, _ = w.Write(value)
		case http.MethodPut:
			value, err := io.ReadAll(io.LimitReader(r.Body, 1025))
			if err != nil {
				t.Error(err)
				http.Error(w, "read", http.StatusInternalServerError)
				return
			}
			objects[r.URL.Path] = value
			w.WriteHeader(http.StatusOK)
		case http.MethodDelete:
			delete(objects, r.URL.Path)
			w.WriteHeader(http.StatusNoContent)
		default:
			http.Error(w, "method", http.StatusMethodNotAllowed)
		}
	}))
	defer server.Close()

	store, err := NewS3(S3Config{
		Endpoint: server.URL, Region: "us-east-1", Bucket: "vlt-projects",
		AccessKeyID: "access", SecretAccessKey: "test-secret-key",
		TempDirectory: t.TempDir(), MaximumBytes: 1024, HTTPClient: server.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	expected := ExpectedObject{SHA256: testSHA256, Bytes: 5, ContentType: "audio/wav"}
	if err := store.VerifyAndPromote(context.Background(), "uploads/project/upload",
		"blobs/8e/"+testSHA256, expected); err != nil {
		t.Fatalf("valid object was rejected: %v", err)
	}
	mutex.Lock()
	final := string(objects["/vlt-projects/blobs/8e/"+testSHA256])
	mutex.Unlock()
	if final != "alpha" {
		t.Fatalf("final object was not promoted exactly: %q", final)
	}

	mutex.Lock()
	objects["/vlt-projects/uploads/project/bad"] = []byte("wrong")
	mutex.Unlock()
	if err := store.VerifyAndPromote(context.Background(), "uploads/project/bad",
		"blobs/00/"+strings.Repeat("0", 64), expected); !errors.Is(err, ErrInvalidObject) {
		t.Fatalf("checksum mismatch was accepted: %v", err)
	}
}

func TestS3RejectsUnsafeConfigurationAndKeys(t *testing.T) {
	_, err := NewS3(S3Config{
		Endpoint: "https://user:password@objects.example.test/path", Region: "us-east-1",
		Bucket: "Bad_Bucket", AccessKeyID: "access", SecretAccessKey: "secret",
		TempDirectory: t.TempDir(), MaximumBytes: 1024,
	})
	if !errors.Is(err, ErrInvalidConfiguration) {
		t.Fatalf("unsafe configuration was accepted: %v", err)
	}
	_, err = NewS3(S3Config{
		Endpoint: "https://objects.example.test", Region: "us-east-1",
		Bucket: "vlt-projects", AccessKeyID: "access", SecretAccessKey: "secret",
		TempDirectory: t.TempDir(), MaximumBytes: MaximumObjectBytes + 1,
	})
	if !errors.Is(err, ErrInvalidConfiguration) {
		t.Fatalf("maximum beyond the bounded multipart layout was accepted: %v", err)
	}
	store, err := NewS3(S3Config{
		Endpoint: "https://objects.example.test", Region: "us-east-1",
		Bucket: "vlt-projects", AccessKeyID: "access", SecretAccessKey: "secret",
		TempDirectory: t.TempDir(), MaximumBytes: 1024,
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"../secret", "/absolute", "uploads//double", "uploads/File Name.wav"} {
		if _, err := store.PresignGet(context.Background(), key, time.Minute); !errors.Is(err, ErrInvalidConfiguration) {
			t.Errorf("unsafe key %q was accepted: %v", key, err)
		}
	}
}
