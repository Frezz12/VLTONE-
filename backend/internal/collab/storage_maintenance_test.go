package collab

import (
	"context"
	"errors"
	"testing"
	"time"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

type maintenanceFakeStore struct {
	aborts    []string
	deletes   []string
	abortErr  error
	deleteErr error
}

func (f *maintenanceFakeStore) PresignPut(context.Context, string,
	objectstore.ExpectedObject, time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignPut")
}

func (f *maintenanceFakeStore) PresignGet(context.Context, string,
	time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignGet")
}

func (f *maintenanceFakeStore) CreateMultipart(context.Context, string,
	objectstore.ExpectedObject) (string, error) {
	return "", errors.New("unexpected CreateMultipart")
}

func (f *maintenanceFakeStore) PresignMultipartPart(context.Context, string,
	string, int, int64, time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignMultipartPart")
}

func (f *maintenanceFakeStore) ListMultipartParts(context.Context, string,
	string, int) ([]objectstore.UploadedPart, error) {
	return nil, errors.New("unexpected ListMultipartParts")
}

func (f *maintenanceFakeStore) CompleteMultipart(context.Context, string,
	string, []objectstore.UploadedPart) error {
	return errors.New("unexpected CompleteMultipart")
}

func (f *maintenanceFakeStore) AbortMultipart(_ context.Context, key,
	providerUploadID string) error {
	f.aborts = append(f.aborts, key+":"+providerUploadID)
	return f.abortErr
}

func (f *maintenanceFakeStore) VerifyAndPromote(context.Context, string,
	string, objectstore.ExpectedObject) error {
	return errors.New("unexpected VerifyAndPromote")
}

func (f *maintenanceFakeStore) Delete(_ context.Context, key string) error {
	f.deletes = append(f.deletes, key)
	return f.deleteErr
}

func TestCleanupExpiredMultipartAbortsBeforeDeletingStaging(t *testing.T) {
	fake := &maintenanceFakeStore{}
	upload := model.UploadSession{
		ObjectKey: "uploads/project/upload", UploadMode: UploadModeMultipart,
		ProviderUploadID: "provider-id", MultipartState: MultipartStateAborting,
	}
	if err := cleanupUploadObject(context.Background(), fake, upload); err != nil {
		t.Fatal(err)
	}
	if len(fake.aborts) != 1 || fake.aborts[0] != "uploads/project/upload:provider-id" ||
		len(fake.deletes) != 1 || fake.deletes[0] != upload.ObjectKey {
		t.Fatalf("cleanup order/state is incomplete: aborts=%v deletes=%v",
			fake.aborts, fake.deletes)
	}
}

func TestCleanupExpiredMultipartRetriesProviderFailureSafely(t *testing.T) {
	fake := &maintenanceFakeStore{abortErr: objectstore.ErrProvider}
	upload := model.UploadSession{
		ObjectKey: "uploads/project/upload", UploadMode: UploadModeMultipart,
		ProviderUploadID: "provider-id", MultipartState: MultipartStateOpen,
	}
	if err := cleanupUploadObject(context.Background(), fake, upload); !errors.Is(err, objectstore.ErrProvider) {
		t.Fatalf("provider abort failure was hidden: %v", err)
	}
	if len(fake.deletes) != 0 {
		t.Fatal("staging object was deleted before multipart abort succeeded")
	}
	fake.abortErr = objectstore.ErrMultipartNotFound
	if err := cleanupUploadObject(context.Background(), fake, upload); err != nil {
		t.Fatalf("idempotent missing multipart was not accepted: %v", err)
	}
	if len(fake.deletes) != 1 {
		t.Fatal("retry did not remove staging object")
	}
}

func TestStorageMaintenanceBounds(t *testing.T) {
	for _, limit := range []int{0, MaximumStorageBatch + 1} {
		if err := validateStorageBatch(limit); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid batch %d returned %v", limit, err)
		}
	}
	if err := validateStorageBatch(128); err != nil {
		t.Fatal(err)
	}
}
