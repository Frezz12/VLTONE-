package api

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/google/uuid"
	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
	"vltstudio/backend/internal/quota"
)

const (
	webCookie   = "vlt_web_session"
	adminCookie = "vlt_admin_session"
	maxJSONBody = 2 << 20
	// This is a protocol/product capability, not a runtime feature flag. V1
	// rejects every recording entry point even when a Config is constructed
	// directly by a test or an embedding process.
	cloudRecordingEnabledV1 = false
)

type Server struct {
	Config             config.Config
	DB                 *gorm.DB
	Signer             *auth.Signer
	Quota              quota.Service
	Collab             *collab.Store
	CollabAssets       *collab.AssetService
	Rooms              collab.RoomBus
	Hashes             *collab.HashCoordinator
	operationSequence  *operationSequencer
	metrics            *collaborationMetrics
	collabAllowedUsers map[uuid.UUID]bool
	limiter            *rateLimiter
	trustedProxies     []*net.IPNet
}

func New(cfg config.Config, db *gorm.DB) (*Server, error) {
	directories := []string{
		filepath.Join(cfg.StorageRoot, "bugs"),
		filepath.Join(cfg.StorageRoot, "crashes"),
		filepath.Join(cfg.StorageRoot, "releases"),
	}
	collaborationTemp := filepath.Join(cfg.StorageRoot, "collaboration-objects")
	if cfg.CollaborationEnabled {
		directories = append(directories, collaborationTemp)
	}
	for _, directory := range directories {
		if err := os.MkdirAll(directory, 0o700); err != nil {
			return nil, fmt.Errorf("create storage: %w", err)
		}
	}
	var trustedProxies []*net.IPNet
	for _, raw := range cfg.TrustedProxyCIDRs {
		_, network, err := net.ParseCIDR(raw)
		if err != nil {
			return nil, fmt.Errorf("invalid TRUSTED_PROXY_CIDRS entry %q: %w", raw, err)
		}
		trustedProxies = append(trustedProxies, network)
	}
	var collaborationAssets *collab.AssetService
	if cfg.CollaborationEnabled {
		objects, err := objectstore.NewS3(objectstore.S3Config{
			Endpoint: cfg.CollabObjectEndpoint, Region: cfg.CollabObjectRegion,
			Bucket: cfg.CollabObjectBucket, AccessKeyID: cfg.CollabObjectAccessKeyID,
			SecretAccessKey: cfg.CollabObjectSecretAccessKey,
			SessionToken:    cfg.CollabObjectSessionToken, TempDirectory: collaborationTemp,
			MaximumBytes: cfg.CollabMaximumObjectBytes,
		})
		if err != nil {
			return nil, fmt.Errorf("configure collaboration object storage: %w", err)
		}
		collaborationAssets = collab.NewAssetService(db, objects,
			time.Duration(cfg.CollabUploadURLSeconds)*time.Second,
			time.Duration(cfg.CollabDownloadURLSeconds)*time.Second,
			time.Duration(cfg.CollabUploadSessionSeconds)*time.Second,
			cfg.CollabMaximumObjectBytes, cfg.CollabMultipartThresholdBytes,
			cfg.CollabMultipartPartBytes, cfg.CollabMultipartURLBatch)
		collaborationAssets.ConfigureLimits(cfg.CollabProjectQuotaBytes,
			cfg.CollabUserQuotaBytes, cfg.CollabMaxOpenUploadsPerUser,
			cfg.CollabMaxOpenUploadsPerProject, cfg.CollabVerifyWorkers,
			cfg.CollabMaxVerifyPerUser)
	}
	allowedUsers := make(map[uuid.UUID]bool, len(cfg.CollabAllowedUserIDs))
	for _, userID := range cfg.CollabAllowedUserIDs {
		allowedUsers[userID] = true
	}
	return &Server{
		Config: cfg, DB: db, Signer: auth.NewSigner(cfg.SigningSeed),
		Quota: quota.Service{DB: db, GlobalMonthlyLimit: cfg.AIGlobalMonthlyLimit}, limiter: newRateLimiter(),
		Collab:             collab.NewStore(db, cfg.CollabMaxParticipants),
		CollabAssets:       collaborationAssets,
		Rooms:              collab.NewInProcessRoomBus(),
		Hashes:             collab.NewHashCoordinator(),
		operationSequence:  newOperationSequencer(),
		metrics:            &collaborationMetrics{},
		collabAllowedUsers: allowedUsers,
		trustedProxies:     trustedProxies,
	}, nil
}

func (s *Server) Router() http.Handler {
	r := chi.NewRouter()
	r.Use(middleware.RequestID)
	r.Use(s.trustedRealIP)
	r.Use(middleware.Recoverer)
	r.Use(s.requestTimeout)
	r.Use(s.securityHeaders)
	r.Get("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	r.Get("/readyz", s.ready)
	r.Get("/metrics", s.collaborationMetrics)
	r.Get("/v1/meta", s.meta)
	r.Get("/v1/releases", s.publicReleases)
	r.Get("/v1/releases/latest", s.latestRelease)
	r.Get("/v1/releases/{version}", s.publicRelease)
	r.Get("/v1/releases/{version}/download/{kind}", s.downloadReleaseArtifact)
	r.Get("/v1/releases/{version}/screenshots/{screenshotID}", s.releaseScreenshot)

	r.Route("/v1/web/auth", func(r chi.Router) {
		r.With(s.requireOrigin(false)).Post("/register", s.register)
		r.With(s.requireOrigin(false)).Post("/login", s.webLogin)
		r.With(s.requireOrigin(false)).Post("/password-reset/request", s.passwordResetRequest)
		r.With(s.requireOrigin(false)).Post("/password-reset/confirm", s.passwordResetConfirm)
		r.With(s.webAuth, s.webCSRF).Post("/logout", s.webLogout)
	})
	r.Group(func(r chi.Router) {
		r.Use(s.webAuth)
		r.Get("/v1/me", s.me)
		r.Get("/v1/me/quota", s.meQuota)
		r.Get("/v1/me/devices", s.meDevices)
		r.With(s.webCSRF).Delete("/v1/me/devices/{deviceID}", s.revokeOwnDevice)
		r.With(s.webCSRF).Post("/v1/bug-reports", s.createBugReport)
	})

	r.Route("/v1/desktop/auth", func(r chi.Router) {
		r.Post("/login", s.desktopLogin)
		r.Post("/refresh", s.desktopRefresh)
		r.With(s.desktopAuth).Post("/logout", s.desktopLogout)
	})
	r.Group(func(r chi.Router) {
		r.Use(s.desktopOrReporterAuth)
		r.Post("/v1/desktop/telemetry/batch", s.telemetryBatch)
		r.Post("/v1/desktop/crashes", s.createCrashReport)
	})
	r.Group(func(r chi.Router) {
		r.Use(s.desktopAuth)
		r.Get("/v1/desktop/me", s.desktopMe)
		r.Get("/v1/desktop/capabilities", s.desktopCapabilities)
		r.Group(func(r chi.Router) {
			r.Use(s.requireCollaborationAccess)
			r.Post("/v1/desktop/project-invites/accept", s.acceptProjectInvite)
			r.Get("/v1/desktop/projects", s.cloudProjects)
			r.Post("/v1/desktop/projects", s.createCloudProject)
			r.Route("/v1/desktop/projects", func(r chi.Router) {
				r.Get("/{projectID}", s.cloudProject)
				r.Patch("/{projectID}", s.updateCloudProject)
				r.Delete("/{projectID}", s.archiveCloudProject)
				r.Post("/{projectID}/publish", s.publishCloudProject)
				r.Get("/{projectID}/bootstrap", s.bootstrapCloudProject)
				r.Get("/{projectID}/members", s.projectMembers)
				r.Put("/{projectID}/members/{userID}", s.putProjectMember)
				r.Delete("/{projectID}/members/{userID}", s.removeProjectMember)
				r.Post("/{projectID}/ownership", s.transferProjectOwnership)
				r.Get("/{projectID}/invites", s.projectInvites)
				r.Post("/{projectID}/invites", s.createProjectInvite)
				r.Delete("/{projectID}/invites/{inviteID}", s.revokeProjectInvite)
				r.Post("/{projectID}/ops", s.appendProjectOperation)
				r.Get("/{projectID}/ops/{opID}", s.projectOperationStatus)
				r.Post("/{projectID}/asset-uploads/prepare", s.prepareProjectAssetUpload)
				r.Post("/{projectID}/asset-uploads/{uploadID}/complete", s.completeProjectAssetUpload)
				r.Delete("/{projectID}/uploads/{uploadID}", s.abortProjectUpload)
				r.Post("/{projectID}/snapshot-uploads/prepare", s.prepareProjectSnapshotUpload)
				r.Post("/{projectID}/snapshot-uploads/{uploadID}/complete", s.completeProjectSnapshotUpload)
				r.Get("/{projectID}/assets/{assetID}/download", s.projectAssetDownload)
				r.Get("/{projectID}/snapshots/{snapshotID}/download", s.projectSnapshotDownload)
				r.Get("/{projectID}/live", s.collaborationLive)
				r.Get("/{projectID}/sessions/active", s.activeProjectSession)
				r.Post("/{projectID}/sessions", s.startProjectSession)
				r.Post("/{projectID}/sessions/{sessionID}/join", s.joinProjectSession)
				r.Post("/{projectID}/sessions/{sessionID}/leave", s.leaveProjectSession)
				r.Post("/{projectID}/sessions/{sessionID}/heartbeat", s.heartbeatProjectSession)
				r.Post("/{projectID}/sessions/{sessionID}/host", s.handoffProjectSessionHost)
				r.Delete("/{projectID}/sessions/{sessionID}", s.endProjectSession)
				r.Post("/{projectID}/sessions/{sessionID}/leases", s.acquireTrackLease)
				r.Patch("/{projectID}/sessions/{sessionID}/leases/{leaseID}", s.renewTrackLease)
				r.Delete("/{projectID}/sessions/{sessionID}/leases/{leaseID}", s.releaseTrackLease)
			})
		})
		r.Get("/v1/desktop/ai/models", s.desktopAIModels)
		r.Post("/v1/desktop/ai/models/{modelID}/lease", s.leaseAIModel)
		r.Post("/v1/desktop/ai/reservations/{reservationID}/settle", s.settleAIReservation)
		// Kept for older desktop builds. Current builds use a checked one-use
		// lease and contact the provider directly from the user's device.
		r.Post("/v1/desktop/ai/models/{modelID}/invoke", s.aiProxy)
		// The assistant's instructions, edited in the admin panel. Fetched on
		// sign-in and every few hours; the ETag makes the usual answer a 304.
		r.Get("/v1/desktop/ai/prompts", s.promptPack)
	})

	r.Route("/v1/admin/auth", func(r chi.Router) {
		r.With(s.requireOrigin(true)).Post("/login", s.adminLogin)
		r.With(s.adminAuth, s.adminCSRF).Post("/logout", s.adminLogout)
	})
	r.Group(func(r chi.Router) {
		r.Use(s.adminAuth)
		r.Get("/v1/admin/me", s.adminMe)
		r.Get("/v1/admin/dashboard", s.adminDashboard)
		r.Get("/v1/admin/users", s.adminUsers)
		r.Get("/v1/admin/users/{userID}", s.adminUser)
		r.Get("/v1/admin/users/{userID}/telemetry", s.adminUserTelemetry)
		r.Get("/v1/admin/users/{userID}/ledger", s.adminUserLedger)
		r.With(s.adminCSRF).Put("/v1/admin/users/{userID}/collaboration-access", s.adminSetCollaborationAccess)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/suspend", s.adminSuspendUser)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/activate", s.adminActivateUser)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/tokens/add", s.adminAddTokens)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/tokens/reset", s.adminResetTokens)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/devices/{deviceID}/revoke", s.adminRevokeDevice)
		r.With(s.adminCSRF).Post("/v1/admin/users/{userID}/sessions/revoke", s.adminRevokeSessions)
		r.With(s.adminCSRF).Delete("/v1/admin/users/{userID}/diagnostics", s.adminDeleteDiagnostics)
		r.With(s.adminCSRF).Delete("/v1/admin/users/{userID}", s.adminDeleteUser)
		r.Get("/v1/admin/bugs", s.adminBugs)
		r.With(s.adminCSRF).Patch("/v1/admin/bugs/{bugID}", s.adminUpdateBug)
		r.Get("/v1/admin/bugs/{bugID}/attachments/{attachmentID}", s.adminBugAttachment)
		r.Get("/v1/admin/crashes", s.adminCrashes)
		r.Get("/v1/admin/crashes/{crashID}/artifact", s.adminCrashArtifact)
		r.Get("/v1/admin/audit", s.adminAudit)
		r.Get("/v1/admin/releases", s.adminReleases)
		r.Get("/v1/admin/releases/{releaseID}", s.adminRelease)
		r.With(s.adminCSRF).Post("/v1/admin/releases", s.adminCreateRelease)
		r.With(s.adminCSRF).Put("/v1/admin/releases/{releaseID}", s.adminUpdateRelease)
		r.With(s.adminCSRF).Delete("/v1/admin/releases/{releaseID}", s.adminDeleteRelease)
		r.With(s.adminCSRF).Post("/v1/admin/releases/{releaseID}/publish", s.adminPublishRelease)
		r.With(s.adminCSRF).Put("/v1/admin/releases/{releaseID}/artifacts/{kind}", s.adminUploadReleaseArtifact)
		r.With(s.adminCSRF).Delete("/v1/admin/releases/{releaseID}/artifacts/{kind}", s.adminDeleteReleaseArtifact)
		r.With(s.adminCSRF).Post("/v1/admin/releases/{releaseID}/screenshots", s.adminUploadReleaseScreenshot)
		r.With(s.adminCSRF).Put("/v1/admin/releases/{releaseID}/screenshots/{screenshotID}", s.adminUpdateReleaseScreenshot)
		r.With(s.adminCSRF).Delete("/v1/admin/releases/{releaseID}/screenshots/{screenshotID}", s.adminDeleteReleaseScreenshot)
		r.Get("/v1/admin/releases/{releaseID}/screenshots/{screenshotID}/file", s.adminReleaseScreenshotFile)
		r.Get("/v1/admin/ai/models", s.adminAIModels)
		r.With(s.adminCSRF).Post("/v1/admin/ai/models", s.adminCreateAIModel)
		r.With(s.adminCSRF).Put("/v1/admin/ai/models/{modelID}", s.adminUpdateAIModel)
		r.With(s.adminCSRF).Delete("/v1/admin/ai/models/{modelID}", s.adminDeleteAIModel)
		r.Get("/v1/admin/ai/prompts", s.adminPrompts)
		r.Get("/v1/admin/ai/prompts/{promptID}", s.adminPrompt)
		r.Get("/v1/admin/ai/prompts/{promptID}/revisions", s.adminPromptRevisions)
		r.With(s.adminCSRF).Post("/v1/admin/ai/prompts", s.adminCreatePrompt)
		r.With(s.adminCSRF).Put("/v1/admin/ai/prompts/{promptID}", s.adminUpdatePrompt)
		r.With(s.adminCSRF).Post("/v1/admin/ai/prompts/{promptID}/revert", s.adminRevertPrompt)
		r.With(s.adminCSRF).Delete("/v1/admin/ai/prompts/{promptID}", s.adminDeletePrompt)
	})
	return r
}

func (s *Server) requestTimeout(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet &&
			strings.HasPrefix(r.URL.Path, "/v1/desktop/projects/") &&
			strings.HasSuffix(r.URL.Path, "/live") {
			// A WebSocket owns its own handshake, read, write, ping and idle
			// deadlines. Applying the REST timeout would kill healthy rooms.
			next.ServeHTTP(w, r)
			return
		}
		duration := 4 * time.Minute
		if r.Method == http.MethodPost && strings.HasPrefix(r.URL.Path, "/v1/desktop/projects/") &&
			(strings.Contains(r.URL.Path, "/asset-uploads/") || strings.Contains(r.URL.Path, "/snapshot-uploads/")) &&
			strings.HasSuffix(r.URL.Path, "/complete") {
			duration = 30 * time.Minute
		}
		if r.Method == http.MethodPut && strings.HasPrefix(r.URL.Path, "/v1/admin/releases/") && strings.Contains(r.URL.Path, "/artifacts/") {
			duration = 30 * time.Minute
		}
		middleware.Timeout(duration)(next).ServeHTTP(w, r)
	})
}

func (s *Server) trustedRealIP(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		peerText := requestIP(r)
		peer := net.ParseIP(peerText)
		trusted := false
		for _, network := range s.trustedProxies {
			if peer != nil && network.Contains(peer) {
				trusted = true
				break
			}
		}
		if trusted {
			candidate := strings.TrimSpace(strings.Split(r.Header.Get("X-Forwarded-For"), ",")[0])
			if candidate == "" {
				candidate = strings.TrimSpace(r.Header.Get("X-Real-IP"))
			}
			if parsed := net.ParseIP(candidate); parsed != nil {
				r.RemoteAddr = parsed.String()
			}
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) requireOrigin(admin bool) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !s.originAllowed(r, admin) {
				writeError(w, r, http.StatusForbidden, "origin_failed", "Request origin could not be verified.", nil)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

func (s *Server) meta(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"service": "vlt-studio", "api_version": "v1",
		"consent_version": s.Config.ConsentVersion,
		"offline_hours":   72, "access_token_minutes": 15,
		"public_key": s.Signer.PublicKeyBase64(),
		"collaboration": map[string]any{
			"enabled": s.Config.CollaborationEnabled, "protocol": collab.CollaborationProtocol,
			"project_format":   collab.CollaborationProjectFormatVersion,
			"command_schema":   collab.CollaborationCommandSchemaVersion,
			"max_participants": s.Config.CollabMaxParticipants,
			"recording":        cloudRecordingEnabledV1,
		},
	})
}

func (s *Server) securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Cache-Control", "no-store")
		next.ServeHTTP(w, r)
	})
}

type APIError struct {
	Code        string            `json:"code"`
	Message     string            `json:"message"`
	FieldErrors map[string]string `json:"field_errors,omitempty"`
	RequestID   string            `json:"request_id,omitempty"`
}

func writeError(w http.ResponseWriter, r *http.Request, status int, code, message string, fields map[string]string) {
	writeJSON(w, status, APIError{
		Code: code, Message: message, FieldErrors: fields,
		RequestID: middleware.GetReqID(r.Context()),
	})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		log.Printf("write response: %v", err)
	}
}

func decodeJSON(w http.ResponseWriter, r *http.Request, out any) bool {
	return decodeJSONWithLimit(w, r, out, maxJSONBody)
}

func decodeJSONWithLimit(w http.ResponseWriter, r *http.Request, out any,
	maximumBytes int64) bool {
	r.Body = http.MaxBytesReader(w, r.Body, maximumBytes)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(out); err != nil {
		writeError(w, r, http.StatusBadRequest, "invalid_request", "Invalid request body.", nil)
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, r, http.StatusBadRequest, "invalid_request", "Only one JSON value is allowed.", nil)
		return false
	}
	return true
}

func parseUUIDParam(w http.ResponseWriter, r *http.Request, name string) (uuid.UUID, bool) {
	id, err := uuid.Parse(chi.URLParam(r, name))
	if err != nil {
		writeError(w, r, http.StatusBadRequest, "invalid_id", "Invalid identifier.", nil)
		return uuid.Nil, false
	}
	return id, true
}

func requestIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil {
		return host
	}
	return r.RemoteAddr
}

func targetHash(id uuid.UUID) string {
	digest := sha256.Sum256([]byte(id.String()))
	return hex.EncodeToString(digest[:])
}

func (s *Server) originAllowed(r *http.Request, admin bool) bool {
	origin := strings.TrimRight(r.Header.Get("Origin"), "/")
	if origin == "" {
		return s.Config.Environment == "development"
	}
	want := s.Config.PublicOrigin
	if admin {
		want = s.Config.AdminOrigin
	}
	return origin == want
}

type contextKey string

const (
	ctxUser          contextKey = "user"
	ctxAdmin         contextKey = "admin"
	ctxDevice        contextKey = "device"
	ctxDesktopClaims contextKey = "desktop_claims"
	ctxWebSession    contextKey = "web_session"
	ctxAdminSession  contextKey = "admin_session"
)

func userFrom(r *http.Request) model.User       { return r.Context().Value(ctxUser).(model.User) }
func adminFrom(r *http.Request) model.AdminUser { return r.Context().Value(ctxAdmin).(model.AdminUser) }
func deviceFrom(r *http.Request) model.Device   { return r.Context().Value(ctxDevice).(model.Device) }

func contextWith(r *http.Request, key contextKey, value any) *http.Request {
	return r.WithContext(context.WithValue(r.Context(), key, value))
}

type rateLimiter struct {
	mu      sync.Mutex
	buckets map[string][]time.Time
}

func newRateLimiter() *rateLimiter { return &rateLimiter{buckets: make(map[string][]time.Time)} }

func (l *rateLimiter) Allow(key string, limit int, window time.Duration, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	cutoff := now.Add(-window)
	values := l.buckets[key][:0]
	for _, value := range l.buckets[key] {
		if value.After(cutoff) {
			values = append(values, value)
		}
	}
	if len(values) >= limit {
		l.buckets[key] = values
		return false
	}
	l.buckets[key] = append(values, now)
	return true
}

func multipartFile(header *multipart.FileHeader, max int64) ([]byte, error) {
	if header.Size > max {
		return nil, errors.New("file too large")
	}
	file, err := header.Open()
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, max+1))
}

func pageLimit(r *http.Request) int {
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 100 {
		return 50
	}
	return limit
}
