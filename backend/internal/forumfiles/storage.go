// Package forumfiles owns the forum data plane independently of collaboration.
package forumfiles

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/objectstore"
)

type Storage struct {
	Root   string
	Remote objectstore.Store
	client *http.Client
}

func New(c config.Config) (*Storage, error) {
	s := &Storage{Root: filepath.Join(c.StorageRoot, "forum"), client: &http.Client{Timeout: 30 * time.Minute, CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse }}}
	if err := os.MkdirAll(s.Root, 0700); err != nil {
		return nil, err
	}
	if c.ForumObjectEndpoint != "" {
		max := c.ForumFileMaxBytes
		if max == 0 {
			max = 1000000000
		}
		o, e := objectstore.NewS3(objectstore.S3Config{Endpoint: c.ForumObjectEndpoint, Region: c.ForumObjectRegion, Bucket: c.ForumObjectBucket, AccessKeyID: c.ForumObjectAccessKeyID, SecretAccessKey: c.ForumObjectSecretAccessKey, SessionToken: c.ForumObjectSessionToken, TempDirectory: filepath.Join(s.Root, "verification"), MaximumBytes: max})
		if e != nil {
			return nil, e
		}
		s.Remote = o
	}
	return s, nil
}
func (s *Storage) Path(key string) (string, error) {
	if key == "" || filepath.IsAbs(key) || strings.Contains(key, "\\") {
		return "", fmt.Errorf("invalid object key")
	}
	clean := filepath.Clean(key)
	if clean == ".." || strings.HasPrefix(clean, "../") {
		return "", fmt.Errorf("invalid object key")
	}
	return filepath.Join(s.Root, clean), nil
}
func Hash(path string) (string, int64, error) {
	f, e := os.Open(path)
	if e != nil {
		return "", 0, e
	}
	defer f.Close()
	h := sha256.New()
	n, e := io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil)), n, e
}
func (s *Storage) Open(ctx context.Context, key string) (io.ReadCloser, error) {
	if s.Remote == nil {
		p, e := s.Path(key)
		if e != nil {
			return nil, e
		}
		return os.Open(p)
	}
	signed, e := s.Remote.PresignGet(ctx, key, 5*time.Minute)
	if e != nil {
		return nil, e
	}
	req, e := http.NewRequestWithContext(ctx, "GET", signed.URL, nil)
	if e != nil {
		return nil, e
	}
	for k, v := range signed.Headers {
		req.Header.Set(k, v)
	}
	res, e := s.client.Do(req)
	if e != nil {
		return nil, e
	}
	if res.StatusCode != 200 {
		res.Body.Close()
		return nil, fmt.Errorf("object read failed: %d", res.StatusCode)
	}
	return res.Body, nil
}
func (s *Storage) Put(ctx context.Context, key, path, contentType string) error {
	hash, n, e := Hash(path)
	if e != nil {
		return e
	}
	expected := objectstore.ExpectedObject{SHA256: hash, Bytes: n, ContentType: contentType}
	if s.Remote == nil {
		dest, e := s.Path(key)
		if e != nil {
			return e
		}
		if e = os.MkdirAll(filepath.Dir(dest), 0700); e != nil {
			return e
		}
		in, e := os.Open(path)
		if e != nil {
			return e
		}
		defer in.Close()
		out, e := os.CreateTemp(filepath.Dir(dest), ".incoming-")
		if e != nil {
			return e
		}
		defer os.Remove(out.Name())
		_, e = io.Copy(out, in)
		closeErr := out.Close()
		if e != nil {
			return e
		}
		if closeErr != nil {
			return closeErr
		}
		return os.Rename(out.Name(), dest)
	}
	staging := key + ".incoming"
	signed, e := s.Remote.PresignPut(ctx, staging, expected, 15*time.Minute)
	if e != nil {
		return e
	}
	f, e := os.Open(path)
	if e != nil {
		return e
	}
	defer f.Close()
	req, e := http.NewRequestWithContext(ctx, "PUT", signed.URL, f)
	if e != nil {
		return e
	}
	req.ContentLength = n
	for k, v := range signed.Headers {
		req.Header.Set(k, v)
	}
	res, e := s.client.Do(req)
	if e != nil {
		return e
	}
	res.Body.Close()
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		return fmt.Errorf("object write failed: %d", res.StatusCode)
	}
	defer s.Remote.Delete(ctx, staging)
	return s.Remote.VerifyAndPromote(ctx, staging, key, expected)
}
func (s *Storage) Delete(ctx context.Context, key string) error {
	if s.Remote != nil {
		return s.Remote.Delete(ctx, key)
	}
	p, e := s.Path(key)
	if e != nil {
		return e
	}
	e = os.Remove(p)
	if os.IsNotExist(e) {
		return nil
	}
	return e
}
func (s *Storage) Serve(w http.ResponseWriter, r *http.Request, key string) {
	if s.Remote == nil {
		p, e := s.Path(key)
		if e != nil {
			http.NotFound(w, r)
			return
		}
		f, e := os.Open(p)
		if e != nil {
			http.NotFound(w, r)
			return
		}
		defer f.Close()
		info, e := f.Stat()
		if e != nil {
			http.NotFound(w, r)
			return
		}
		http.ServeContent(w, r, "", info.ModTime(), f)
		return
	}
	signed, e := s.Remote.PresignGet(r.Context(), key, 5*time.Minute)
	if e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	req, e := http.NewRequestWithContext(r.Context(), "GET", signed.URL, nil)
	if e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	for k, v := range signed.Headers {
		req.Header.Set(k, v)
	}
	if value := r.Header.Get("Range"); value != "" {
		req.Header.Set("Range", value)
	}
	res, e := s.client.Do(req)
	if e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	defer res.Body.Close()
	if res.StatusCode != 200 && res.StatusCode != 206 && res.StatusCode != 416 {
		http.NotFound(w, r)
		return
	}
	for _, k := range []string{"Content-Length", "Content-Range", "Accept-Ranges"} {
		if v := res.Header.Get(k); v != "" {
			w.Header().Set(k, v)
		}
	}
	w.WriteHeader(res.StatusCode)
	io.Copy(w, res.Body)
}
