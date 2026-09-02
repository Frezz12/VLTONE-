package config

import (
	"encoding/base64"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	Environment                     string
	HTTPAddr                        string
	DatabaseURL                     string
	PublicOrigin                    string
	AdminOrigin                     string
	DesktopAPIOrigin                string
	StorageRoot                     string
	SigningSeed                     []byte
	AICredentialsKey                []byte
	ConsentVersion                  string
	AIEnabled                       bool
	AIGlobalMonthlyLimit            int64
	CollaborationEnabled            bool
	CollabMaxParticipants           int
	CollabSnapshotOps               int64
	CollabSnapshotSeconds           int
	CollabSnapshotRetrySeconds      int
	CollabLeaseSeconds              int
	CollabReconnectSeconds          int
	CollabMemberStaleSeconds        int
	CollabReaperBatch               int
	CollabStorageReaperBatch        int
	CollabMaintenanceTimeoutSeconds int
	CollabBlobRetentionSeconds      int
	CollabWSDBTimeoutSeconds        int
	CollabRoomQueueBytes            int64
	CollabObjectEndpoint            string
	CollabObjectRegion              string
	CollabObjectBucket              string
	CollabObjectAccessKeyID         string
	CollabObjectSecretAccessKey     string
	CollabObjectSessionToken        string
	CollabUploadURLSeconds          int
	CollabDownloadURLSeconds        int
	CollabUploadSessionSeconds      int
	CollabMaximumObjectBytes        int64
	CollabMultipartThresholdBytes   int64
	CollabMultipartPartBytes        int64
	CollabMultipartURLBatch         int
	SMTPHost                        string
	SMTPPort                        int
	SMTPUsername                    string
	SMTPPassword                    string
	SMTPFrom                        string
	TrustedProxyCIDRs               []string
}

func Load() (Config, error) {
	c := Config{
		Environment:          env("APP_ENV", "development"),
		HTTPAddr:             env("HTTP_ADDR", ":8080"),
		DatabaseURL:          env("DATABASE_URL", "postgres://vlt:vlt@localhost:5432/vltstudio?sslmode=disable"),
		PublicOrigin:         strings.TrimRight(env("PUBLIC_ORIGIN", "http://localhost:3000"), "/"),
		AdminOrigin:          strings.TrimRight(env("ADMIN_ORIGIN", "http://localhost:3001"), "/"),
		DesktopAPIOrigin:     strings.TrimRight(env("DESKTOP_API_ORIGIN", "http://localhost:8080"), "/"),
		StorageRoot:          env("STORAGE_ROOT", "./storage"),
		ConsentVersion:       env("TELEMETRY_CONSENT_VERSION", "2026-08-23"),
		AIEnabled:            boolEnv("AI_ENABLED", true),
		AIGlobalMonthlyLimit: int64Env("AI_GLOBAL_MONTHLY_TOKEN_LIMIT", 0),
		// Collaboration remains opt-in until command coverage and the
		// production acceptance suite are complete. Keeping all operational
		// limits in configuration also lets a single-instance dogfood deploy be
		// tuned without changing the wire protocol.
		CollaborationEnabled:            boolEnv("COLLABORATION_ENABLED", false),
		CollabMaxParticipants:           int(boundedInt64Env("COLLAB_MAX_PARTICIPANTS", 8, 1, 8)),
		CollabSnapshotOps:               boundedInt64Env("COLLAB_SNAPSHOT_OPS", 500, 1, 100000),
		CollabSnapshotSeconds:           int(boundedInt64Env("COLLAB_SNAPSHOT_SECONDS", 300, 30, 86400)),
		CollabSnapshotRetrySeconds:      int(boundedInt64Env("COLLAB_SNAPSHOT_RETRY_SECONDS", 15, 5, 300)),
		CollabLeaseSeconds:              int(boundedInt64Env("COLLAB_LEASE_SECONDS", 15, 5, 120)),
		CollabReconnectSeconds:          int(boundedInt64Env("COLLAB_RECONNECT_SECONDS", 30, 5, 300)),
		CollabMemberStaleSeconds:        int(boundedInt64Env("COLLAB_MEMBER_STALE_SECONDS", 45, 30, 600)),
		CollabReaperBatch:               int(boundedInt64Env("COLLAB_REAPER_BATCH", 256, 1, 1000)),
		CollabStorageReaperBatch:        int(boundedInt64Env("COLLAB_STORAGE_REAPER_BATCH", 128, 1, 1000)),
		CollabMaintenanceTimeoutSeconds: int(boundedInt64Env("COLLAB_MAINTENANCE_TIMEOUT_SECONDS", 20, 5, 120)),
		CollabBlobRetentionSeconds:      int(boundedInt64Env("COLLAB_BLOB_RETENTION_SECONDS", 604800, 3600, 7776000)),
		CollabWSDBTimeoutSeconds:        int(boundedInt64Env("COLLAB_WS_DB_TIMEOUT_SECONDS", 15, 3, 60)),
		CollabRoomQueueBytes:            boundedInt64Env("COLLAB_ROOM_QUEUE_BYTES", 8<<20, 2<<20, 64<<20),
		CollabObjectEndpoint:            strings.TrimRight(strings.TrimSpace(os.Getenv("COLLAB_OBJECT_ENDPOINT")), "/"),
		CollabObjectRegion:              env("COLLAB_OBJECT_REGION", "us-east-1"),
		CollabObjectBucket:              strings.TrimSpace(os.Getenv("COLLAB_OBJECT_BUCKET")),
		CollabObjectAccessKeyID:         strings.TrimSpace(os.Getenv("COLLAB_OBJECT_ACCESS_KEY_ID")),
		CollabObjectSecretAccessKey:     os.Getenv("COLLAB_OBJECT_SECRET_ACCESS_KEY"),
		CollabObjectSessionToken:        os.Getenv("COLLAB_OBJECT_SESSION_TOKEN"),
		CollabUploadURLSeconds:          int(boundedInt64Env("COLLAB_UPLOAD_URL_SECONDS", 900, 60, 3600)),
		CollabDownloadURLSeconds:        int(boundedInt64Env("COLLAB_DOWNLOAD_URL_SECONDS", 300, 30, 900)),
		CollabUploadSessionSeconds:      int(boundedInt64Env("COLLAB_UPLOAD_SESSION_SECONDS", 86400, 900, 604800)),
		CollabMaximumObjectBytes:        boundedInt64Env("COLLAB_MAX_OBJECT_BYTES", 8<<30, 1<<20, 64<<30),
		CollabMultipartThresholdBytes:   boundedInt64Env("COLLAB_MULTIPART_THRESHOLD_BYTES", 64<<20, 10<<20, 5<<30),
		CollabMultipartPartBytes:        boundedInt64Env("COLLAB_MULTIPART_PART_BYTES", 16<<20, 5<<20, 5<<30),
		CollabMultipartURLBatch:         int(boundedInt64Env("COLLAB_MULTIPART_URL_BATCH", 100, 1, 200)),
		SMTPHost:                        os.Getenv("SMTP_HOST"),
		SMTPPort:                        int(int64Env("SMTP_PORT", 587)),
		SMTPUsername:                    os.Getenv("SMTP_USERNAME"),
		SMTPPassword:                    os.Getenv("SMTP_PASSWORD"),
		SMTPFrom:                        env("SMTP_FROM", "VLT Studio <no-reply@example.com>"),
		TrustedProxyCIDRs:               csv(env("TRUSTED_PROXY_CIDRS", "")),
	}
	root, err := filepath.Abs(c.StorageRoot)
	if err != nil {
		return Config{}, fmt.Errorf("storage root: %w", err)
	}
	c.StorageRoot = root
	seedText := strings.TrimSpace(os.Getenv("AUTH_SIGNING_SEED"))
	if seedText != "" {
		c.SigningSeed, err = base64.StdEncoding.DecodeString(seedText)
		if err != nil || len(c.SigningSeed) != 32 {
			return Config{}, errors.New("AUTH_SIGNING_SEED must be a base64-encoded 32-byte seed")
		}
	}
	credentialsKeyText := strings.TrimSpace(os.Getenv("AI_CREDENTIALS_KEY"))
	if credentialsKeyText != "" {
		c.AICredentialsKey, err = base64.StdEncoding.DecodeString(credentialsKeyText)
		if err != nil || len(c.AICredentialsKey) != 32 {
			return Config{}, errors.New("AI_CREDENTIALS_KEY must be a base64-encoded 32-byte key")
		}
	}
	for _, raw := range []string{c.PublicOrigin, c.AdminOrigin, c.DesktopAPIOrigin} {
		parsed, parseErr := url.Parse(raw)
		if parseErr != nil || parsed.Scheme == "" || parsed.Host == "" {
			return Config{}, fmt.Errorf("invalid origin %q", raw)
		}
		if c.Environment == "production" && parsed.Scheme != "https" {
			return Config{}, fmt.Errorf("production origin must use HTTPS: %s", raw)
		}
	}
	if c.CollaborationEnabled {
		endpoint, parseErr := url.Parse(c.CollabObjectEndpoint)
		if parseErr != nil || endpoint.Scheme == "" || endpoint.Host == "" || endpoint.User != nil ||
			(endpoint.Scheme != "http" && endpoint.Scheme != "https") ||
			(endpoint.Path != "" && endpoint.Path != "/") || endpoint.RawQuery != "" || endpoint.Fragment != "" {
			return Config{}, errors.New("COLLAB_OBJECT_ENDPOINT must be an absolute HTTP(S) origin without credentials, path, query or fragment")
		}
		if c.Environment == "production" && endpoint.Scheme != "https" {
			return Config{}, errors.New("COLLAB_OBJECT_ENDPOINT must use HTTPS in production")
		}
		if !validS3Bucket(c.CollabObjectBucket) {
			return Config{}, errors.New("COLLAB_OBJECT_BUCKET must be a DNS-compatible bucket name")
		}
		if c.CollabObjectAccessKeyID == "" || c.CollabObjectSecretAccessKey == "" {
			return Config{}, errors.New("COLLAB_OBJECT_ACCESS_KEY_ID and COLLAB_OBJECT_SECRET_ACCESS_KEY are required when collaboration is enabled")
		}
		if strings.ContainsAny(c.CollabObjectAccessKeyID, "\r\n") ||
			strings.ContainsAny(c.CollabObjectSecretAccessKey, "\r\n") ||
			strings.ContainsAny(c.CollabObjectSessionToken, "\r\n") ||
			strings.ContainsAny(c.CollabObjectRegion, "\r\n/") {
			return Config{}, errors.New("collaboration object-storage configuration contains invalid characters")
		}
	}
	if c.Environment == "production" {
		if len(c.SigningSeed) != 32 {
			return Config{}, errors.New("AUTH_SIGNING_SEED is required in production")
		}
		if c.AIGlobalMonthlyLimit <= 0 {
			return Config{}, errors.New("AI_GLOBAL_MONTHLY_TOKEN_LIMIT must be positive in production")
		}
		if c.SMTPHost == "" || c.SMTPPort <= 0 {
			return Config{}, errors.New("SMTP_HOST and a valid SMTP_PORT are required in production")
		}
	}
	return c, nil
}

func env(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

func int64Env(name string, fallback int64) int64 {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func boundedInt64Env(name string, fallback, minimum, maximum int64) int64 {
	value := int64Env(name, fallback)
	if value < minimum || value > maximum {
		return fallback
	}
	return value
}

func boolEnv(name string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func csv(value string) []string {
	var out []string
	for _, item := range strings.Split(value, ",") {
		if item = strings.TrimSpace(item); item != "" {
			out = append(out, item)
		}
	}
	return out
}

func validS3Bucket(value string) bool {
	if len(value) < 3 || len(value) > 63 || strings.HasPrefix(value, ".") ||
		strings.HasSuffix(value, ".") || strings.HasPrefix(value, "-") ||
		strings.HasSuffix(value, "-") || strings.Contains(value, "..") {
		return false
	}
	for _, character := range value {
		if (character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') || character == '.' || character == '-' {
			continue
		}
		return false
	}
	return true
}
