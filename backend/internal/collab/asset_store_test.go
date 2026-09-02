package collab

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/google/uuid"
	"gorm.io/datatypes"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

func TestNormalizeAssetUploadUsesOpaqueKeysAndBasenameOnly(t *testing.T) {
	service := &AssetService{MaximumBytes: 1024}
	projectID := uuid.MustParse("11111111-1111-4111-8111-111111111111")
	uploadID := uuid.MustParse("22222222-2222-4222-8222-222222222222")
	assetID := uuid.MustParse("33333333-3333-4333-8333-333333333333")
	actorID := uuid.MustParse("44444444-4444-4444-8444-444444444444")
	deviceID := uuid.MustParse("55555555-5555-4555-8555-555555555555")
	sessionID := uuid.MustParse("66666666-6666-4666-8666-666666666666")
	normalized, err := service.normalizeAssetUpload(PrepareAssetUploadInput{
		ProjectID: projectID, UploadID: uploadID, AssetID: assetID,
		ActorUserID: actorID, DeviceID: deviceID, ActorSessionID: sessionID,
		SHA256: strings.Repeat("a", 64), Bytes: 512, Kind: "audio",
		ContentType: "audio/wav", DisplayName: `C:\\Users\\artist\\Kick.wav`,
	})
	if err != nil {
		t.Fatal(err)
	}
	if normalized.DisplayName != "Kick.wav" {
		t.Fatalf("absolute client path was retained: %q", normalized.DisplayName)
	}
	if key := stagingObjectKey(projectID, uploadID); key !=
		"uploads/11111111-1111-4111-8111-111111111111/22222222-2222-4222-8222-222222222222" {
		t.Fatalf("staging key is not opaque: %q", key)
	}
	if strings.Contains(stagingObjectKey(projectID, uploadID), normalized.DisplayName) {
		t.Fatal("object key contains a client filename")
	}
	if len(normalized.RequestHash) != 64 {
		t.Fatalf("prepare idempotency hash is invalid: %q", normalized.RequestHash)
	}
}

func TestNormalizeUploadsRejectsUnsafeOrOversizedValues(t *testing.T) {
	service := &AssetService{MaximumBytes: 100}
	base := PrepareAssetUploadInput{
		ProjectID: uuid.New(), UploadID: uuid.New(), AssetID: uuid.New(),
		ActorUserID: uuid.New(), DeviceID: uuid.New(), ActorSessionID: uuid.New(),
		SHA256: strings.Repeat("a", 64),
		Bytes:  5, Kind: "audio", ContentType: "audio/wav", DisplayName: "take.wav",
	}
	bad := base
	bad.SHA256 = strings.Repeat("A", 64)
	if _, err := service.normalizeAssetUpload(bad); !errors.Is(err, ErrValidation) {
		t.Fatalf("uppercase checksum was accepted: %v", err)
	}
	bad = base
	bad.Bytes = 101
	if _, err := service.normalizeAssetUpload(bad); !errors.Is(err, ErrValidation) {
		t.Fatalf("oversized upload was accepted: %v", err)
	}
	bad = base
	bad.ContentType = "audio/wav\r\nX-Leak: value"
	if _, err := service.normalizeAssetUpload(bad); !errors.Is(err, ErrValidation) {
		t.Fatalf("header injection content type was accepted: %v", err)
	}
}

func TestMultipartCursorDoesNotChangePrepareIdempotency(t *testing.T) {
	service := &AssetService{MaximumBytes: 1 << 30}
	input := PrepareAssetUploadInput{
		ProjectID: uuid.New(), UploadID: uuid.New(), AssetID: uuid.New(),
		ActorUserID: uuid.New(), DeviceID: uuid.New(), ActorSessionID: uuid.New(),
		SHA256: strings.Repeat("a", 64), Bytes: 256 << 20, Kind: "audio",
		ContentType: "audio/wav", DisplayName: "take.wav", PartNumberStart: 1,
	}
	first, err := service.normalizeAssetUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	input.PartNumberStart = 101
	second, err := service.normalizeAssetUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	if first.RequestHash != second.RequestHash {
		t.Fatal("multipart URL-page cursor changed the upload idempotency identity")
	}
}

func TestSnapshotAssetManifestIsCanonicalAndBoundToUploadIdempotency(t *testing.T) {
	service := &AssetService{MaximumBytes: 1 << 20}
	firstID := uuid.MustParse("11111111-1111-4111-8111-aaaaaaaaaaaa")
	secondID := uuid.MustParse("22222222-2222-4222-8222-bbbbbbbbbbbb")
	input := PrepareSnapshotUploadInput{
		ProjectID: uuid.New(), UploadID: uuid.New(), ActorUserID: uuid.New(),
		DeviceID: uuid.New(), ActorSessionID: uuid.New(), Seq: 0,
		SchemaVersion: CollaborationProjectFormatVersion,
		SHA256:        strings.Repeat("a", 64), Bytes: 128,
		ContentType: "application/vnd.vltone.project+json",
		AssetIDs:    []string{secondID.String(), firstID.String()},
	}
	first, err := service.normalizeSnapshotUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(first.SnapshotAssetIDs),
		`["11111111-1111-4111-8111-aaaaaaaaaaaa","22222222-2222-4222-8222-bbbbbbbbbbbb"]`; got != want {
		t.Fatalf("manifest was not canonicalized: got %s want %s", got, want)
	}
	input.AssetIDs = []string{firstID.String(), secondID.String()}
	second, err := service.normalizeSnapshotUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	if first.RequestHash != second.RequestHash {
		t.Fatalf("manifest order changed request identity: %s != %s",
			first.RequestHash, second.RequestHash)
	}
	input.AssetIDs = []string{firstID.String()}
	changed, err := service.normalizeSnapshotUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	if changed.RequestHash == first.RequestHash {
		t.Fatal("different snapshot asset manifests shared a request hash")
	}

	for name, values := range map[string][]string{
		"duplicate":    {firstID.String(), firstID.String()},
		"uppercase":    {strings.ToUpper(firstID.String())},
		"braced":       {"{" + firstID.String() + "}"},
		"nil uuid":     {uuid.Nil.String()},
		"over maximum": make([]string, MaximumSnapshotAssets+1),
	} {
		t.Run(name, func(t *testing.T) {
			input.AssetIDs = values
			if _, err := service.normalizeSnapshotUpload(input); !errors.Is(err, ErrValidation) {
				t.Fatalf("invalid manifest returned %v", err)
			}
		})
	}
}

func TestLegacyEmptySnapshotManifestRetryRemainsIdempotent(t *testing.T) {
	service := &AssetService{MaximumBytes: 1 << 20}
	input := PrepareSnapshotUploadInput{
		ProjectID: uuid.New(), UploadID: uuid.New(), ActorUserID: uuid.New(),
		DeviceID: uuid.New(), ActorSessionID: uuid.New(), Seq: 0,
		SchemaVersion: CollaborationProjectFormatVersion,
		SHA256:        strings.Repeat("b", 64), Bytes: 128,
		ContentType: "application/vnd.vltone.project+json", AssetIDs: []string{},
	}
	normalized, err := service.normalizeSnapshotUpload(input)
	if err != nil {
		t.Fatal(err)
	}
	legacy := normalized
	legacy.RequestHash = legacyUploadRequestHash(legacy)
	existing := model.UploadSession{
		RequestHash:      legacy.RequestHash,
		SnapshotAssetIDs: datatypes.JSON([]byte("[]")),
	}
	if !uploadRequestMatches(existing, normalized) {
		t.Fatal("migration broke an in-flight legacy empty-manifest retry")
	}
	normalized.SnapshotAssetIDs = datatypes.JSON([]byte(
		`["11111111-1111-4111-8111-111111111111"]`))
	normalized.RequestHash = uploadRequestHash(normalized)
	if uploadRequestMatches(existing, normalized) {
		t.Fatal("legacy hash bypassed a non-empty snapshot manifest")
	}
}

func TestFinalObjectKeyIsContentAddressed(t *testing.T) {
	digest := strings.Repeat("b", 64)
	if key := finalObjectKey(digest); key != "blobs/bb/"+digest {
		t.Fatalf("unexpected final object key: %q", key)
	}
}

func TestCommandAssetRequirementsIncludeAtomicBatchTakes(t *testing.T) {
	payload := json.RawMessage(`{"commands":[{"kind":"track.setProperty","payload":{}},{"kind":"take.add","payload":{"take":{"asset":{"assetId":"33333333-3333-4333-8333-333333333333","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","kind":"audio","byteSize":512}}}}]}`)
	requirements, err := commandAssetRequirements("batch", payload, true)
	if err != nil {
		t.Fatal(err)
	}
	if len(requirements) != 1 || requirements[0].AssetID.String() !=
		"33333333-3333-4333-8333-333333333333" || requirements[0].Bytes != 512 {
		t.Fatalf("batch asset requirements were not derived: %+v", requirements)
	}
}

func TestSubmittedMultipartManifestMustExactlyMatchProviderOrderAndETags(t *testing.T) {
	provider := []objectstore.UploadedPart{
		{PartNumber: 1, ETag: `"part-one"`, Bytes: 5 << 20},
		{PartNumber: 2, ETag: `"part-two"`, Bytes: 7},
	}
	valid := []CompleteMultipartPart{
		{PartNumber: 1, ETag: `"part-one"`},
		{PartNumber: 2, ETag: `"part-two"`},
	}
	if !submittedManifestMatches(valid, provider) {
		t.Fatal("exact provider-observed multipart manifest was rejected")
	}
	for name, candidate := range map[string][]CompleteMultipartPart{
		"missing part": valid[:1],
		"reordered": {
			{PartNumber: 2, ETag: `"part-two"`},
			{PartNumber: 1, ETag: `"part-one"`},
		},
		"changed etag": {
			{PartNumber: 1, ETag: `"different"`},
			{PartNumber: 2, ETag: `"part-two"`},
		},
		"control etag": {
			{PartNumber: 1, ETag: "bad\nvalue"},
			{PartNumber: 2, ETag: `"part-two"`},
		},
	} {
		t.Run(name, func(t *testing.T) {
			if submittedManifestMatches(candidate, provider) {
				t.Fatal("untrusted multipart completion manifest was accepted")
			}
		})
	}
}

func TestStoredMultipartManifestIsBoundedAndStrict(t *testing.T) {
	valid, err := decodeMultipartManifest([]byte(`[{"partNumber":1,"byteSize":5242880,"eTag":"\"one\""},{"partNumber":2,"byteSize":7,"eTag":"\"two\""}]`))
	if err != nil || len(valid) != 2 || valid[1].Bytes != 7 {
		t.Fatalf("valid stored manifest was rejected: parts=%+v err=%v", valid, err)
	}
	for name, raw := range map[string]string{
		"object":       `{}`,
		"zero bytes":   `[{"partNumber":1,"byteSize":0,"eTag":"one"}]`,
		"control etag": `[{"partNumber":1,"byteSize":1,"eTag":"bad\nvalue"}]`,
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := decodeMultipartManifest([]byte(raw)); !errors.Is(err, ErrConflict) {
				t.Fatalf("invalid stored manifest was accepted: %v", err)
			}
		})
	}
}
