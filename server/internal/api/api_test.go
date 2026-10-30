package api

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"dropspace/server/internal/store"
)

type testServer struct {
	t       *testing.T
	store   *store.Store
	server  *httptest.Server
	spaceID string
}

func newTestServer(t *testing.T) *testServer {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	spaceID, err := st.EnsureSpace(context.Background(), "test")
	if err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	server := httptest.NewServer(New(st, logger, true))
	value := &testServer{t: t, store: st, server: server, spaceID: spaceID}
	t.Cleanup(func() { server.Close(); st.Close() })
	return value
}

func (s *testServer) pair(name string) string {
	s.t.Helper()
	pairingToken, err := s.store.CreatePairingToken(context.Background(), s.spaceID, time.Minute)
	if err != nil {
		s.t.Fatal(err)
	}
	body := map[string]any{"pairing_token": pairingToken, "display_name": name, "platform": "windows"}
	response := s.request(http.MethodPost, "/v1/pair", "", body, "application/json")
	defer response.Body.Close()
	if response.StatusCode != http.StatusCreated {
		s.t.Fatalf("pair status: %d", response.StatusCode)
	}
	var result struct {
		DeviceToken string `json:"device_token"`
	}
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		s.t.Fatal(err)
	}
	return result.DeviceToken
}

func (s *testServer) request(method, path, token string, body any, contentType string) *http.Response {
	s.t.Helper()
	var reader io.Reader
	if body != nil {
		if raw, ok := body.([]byte); ok {
			reader = bytes.NewReader(raw)
		} else {
			encoded, err := json.Marshal(body)
			if err != nil {
				s.t.Fatal(err)
			}
			reader = bytes.NewReader(encoded)
		}
	}
	request, err := http.NewRequest(method, s.server.URL+path, reader)
	if err != nil {
		s.t.Fatal(err)
	}
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		s.t.Fatal(err)
	}
	return response
}

func sha(value []byte) string {
	digest := sha256.Sum256(value)
	return hex.EncodeToString(digest[:])
}

func fileIntent(uploadID string, content []byte) map[string]any {
	return map[string]any{
		"client_upload_id": uploadID, "kind": "file", "name": "payload.txt", "object_format": "raw",
		"object_size_bytes": len(content), "object_sha256": sha(content),
		"expanded_size_bytes": len(content), "entry_count": 1,
	}
}

func TestRawUploadLifecycleAndIdempotency(t *testing.T) {
	s := newTestServer(t)
	token := s.pair("device A")
	content := []byte("immutable payload")
	intent := fileIntent("upload-0001", content)
	response := s.request(http.MethodPost, "/v1/files", token, intent, "application/json")
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create status: %d", response.StatusCode)
	}
	var created fileMetadata
	if err := json.NewDecoder(response.Body).Decode(&created); err != nil {
		t.Fatal(err)
	}
	response.Body.Close()

	repeated := s.request(http.MethodPost, "/v1/files", token, intent, "application/json")
	if repeated.StatusCode != http.StatusOK {
		t.Fatalf("idempotent status: %d", repeated.StatusCode)
	}
	var same fileMetadata
	_ = json.NewDecoder(repeated.Body).Decode(&same)
	repeated.Body.Close()
	if same.ID != created.ID {
		t.Fatal("idempotent create returned a different logical file")
	}

	bad := append([]byte{}, content...)
	bad[0] ^= 1
	response = s.request(http.MethodPut, "/v1/files/"+created.ID+"/content", token, bad, "application/octet-stream")
	if response.StatusCode != http.StatusUnprocessableEntity {
		t.Fatalf("bad PUT status: %d", response.StatusCode)
	}
	response.Body.Close()

	snapshot := s.request(http.MethodGet, "/v1/snapshot", token, nil, "")
	var before struct {
		Files []fileMetadata `json:"files"`
	}
	_ = json.NewDecoder(snapshot.Body).Decode(&before)
	snapshot.Body.Close()
	if len(before.Files) != 0 {
		t.Fatal("failed upload appeared ready")
	}

	response = s.request(http.MethodPut, "/v1/files/"+created.ID+"/content", token, content, "application/octet-stream")
	if response.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(response.Body)
		t.Fatalf("PUT status: %d %s", response.StatusCode, data)
	}
	response.Body.Close()

	download := s.request(http.MethodGet, "/v1/files/"+created.ID+"/content", token, nil, "")
	bytesRead, _ := io.ReadAll(download.Body)
	download.Body.Close()
	if download.StatusCode != http.StatusOK || !bytes.Equal(bytesRead, content) {
		t.Fatal("download did not match upload")
	}
	if download.Header.Get("ETag") != `"sha256:`+sha(content)+`"` {
		t.Fatal("stable digest ETag missing")
	}

	deleted := s.request(http.MethodDelete, "/v1/files/"+created.ID, token, nil, "")
	if deleted.StatusCode != http.StatusNoContent {
		t.Fatalf("delete status: %d", deleted.StatusCode)
	}
	deleted.Body.Close()
	afterDelete := s.request(http.MethodGet, "/v1/files/"+created.ID+"/content", token, nil, "")
	if afterDelete.StatusCode != http.StatusGone {
		t.Fatalf("deleted GET status: %d", afterDelete.StatusCode)
	}
	afterDelete.Body.Close()
}

func TestAuthenticationAndForgedDeviceField(t *testing.T) {
	s := newTestServer(t)
	token := s.pair("device A")
	unauthorized := s.request(http.MethodGet, "/v1/snapshot", "", nil, "")
	if unauthorized.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthorized status: %d", unauthorized.StatusCode)
	}
	unauthorized.Body.Close()
	intent := fileIntent("upload-0002", []byte("x"))
	intent["upload_device_id"] = "forged"
	forged := s.request(http.MethodPost, "/v1/files", token, intent, "application/json")
	if forged.StatusCode != http.StatusBadRequest {
		t.Fatalf("forged device field status: %d", forged.StatusCode)
	}
	forged.Body.Close()
}

func TestZeroByteUpload(t *testing.T) {
	s := newTestServer(t)
	token := s.pair("device A")
	intent := fileIntent("upload-zero", nil)
	createdResponse := s.request(http.MethodPost, "/v1/files", token, intent, "application/json")
	var created fileMetadata
	_ = json.NewDecoder(createdResponse.Body).Decode(&created)
	createdResponse.Body.Close()
	put := s.request(http.MethodPut, "/v1/files/"+created.ID+"/content", token, []byte{}, "application/octet-stream")
	if put.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(put.Body)
		t.Fatalf("zero PUT: %d %s", put.StatusCode, data)
	}
	put.Body.Close()
}

func TestDeviceRevocation(t *testing.T) {
	s := newTestServer(t)
	adminToken := s.pair("device A")
	revokedToken := s.pair("device B")
	me := s.request(http.MethodGet, "/v1/me", revokedToken, nil, "")
	var identity struct {
		DeviceID string `json:"device_id"`
	}
	if err := json.NewDecoder(me.Body).Decode(&identity); err != nil {
		t.Fatal(err)
	}
	me.Body.Close()
	revoked := s.request(http.MethodDelete, "/v1/devices/"+identity.DeviceID, adminToken, nil, "")
	if revoked.StatusCode != http.StatusNoContent {
		t.Fatalf("revoke status: %d", revoked.StatusCode)
	}
	revoked.Body.Close()
	denied := s.request(http.MethodGet, "/v1/snapshot", revokedToken, nil, "")
	if denied.StatusCode != http.StatusUnauthorized {
		t.Fatalf("revoked token status: %d", denied.StatusCode)
	}
	denied.Body.Close()
}
