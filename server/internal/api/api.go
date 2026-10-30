package api

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"mime"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"dropspace/server/internal/store"
	"dropspace/server/internal/zipcheck"
)

type identityKey struct{}

type API struct {
	store       *store.Store
	log         *slog.Logger
	development bool
	uploadMu    sync.Mutex
	uploading   map[string]struct{}
	pairMu      sync.Mutex
	pairLimits  map[string]pairLimit
}

type pairLimit struct {
	count int
	reset time.Time
}

type errorBody struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	RequestID string `json:"request_id"`
}

type fileMetadata struct {
	ID                string  `json:"id"`
	UploadDeviceID    string  `json:"upload_device_id"`
	Kind              string  `json:"kind"`
	Name              string  `json:"name"`
	ObjectFormat      string  `json:"object_format"`
	ObjectSizeBytes   int64   `json:"object_size_bytes"`
	ObjectSHA256      string  `json:"object_sha256"`
	ExpandedSizeBytes int64   `json:"expanded_size_bytes"`
	EntryCount        int64   `json:"entry_count"`
	TreeSHA256        *string `json:"tree_sha256,omitempty"`
	State             string  `json:"state"`
	CreatedAt         string  `json:"created_at"`
	ReadyAt           *string `json:"ready_at,omitempty"`
	ExpiresAt         *string `json:"expires_at,omitempty"`
}

func New(st *store.Store, logger *slog.Logger, development bool) http.Handler {
	api := &API{store: st, log: logger, development: development, uploading: make(map[string]struct{}), pairLimits: make(map[string]pairLimit)}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", api.health)
	mux.HandleFunc("POST /v1/pair", api.pair)
	mux.Handle("GET /v1/me", api.auth(http.HandlerFunc(api.me)))
	mux.Handle("GET /v1/devices", api.auth(http.HandlerFunc(api.devices)))
	mux.Handle("DELETE /v1/devices/{id}", api.auth(http.HandlerFunc(api.revokeDevice)))
	mux.Handle("POST /v1/files", api.auth(http.HandlerFunc(api.createFile)))
	mux.Handle("PUT /v1/files/{id}/content", api.auth(http.HandlerFunc(api.putContent)))
	mux.Handle("GET /v1/files/{id}/content", api.auth(http.HandlerFunc(api.getContent)))
	mux.Handle("GET /v1/files/{id}/manifest", api.auth(http.HandlerFunc(api.getManifest)))
	mux.Handle("DELETE /v1/files/{id}", api.auth(http.HandlerFunc(api.deleteFile)))
	mux.Handle("GET /v1/snapshot", api.auth(http.HandlerFunc(api.snapshot)))
	mux.Handle("GET /v1/events", api.auth(http.HandlerFunc(api.events)))
	return api.requestLog(mux)
}

func (a *API) requestLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		requestID, _ := store.RandomHex(8)
		response.Header().Set("X-Request-ID", requestID)
		request = request.WithContext(context.WithValue(request.Context(), requestIDKey{}, requestID))
		started := time.Now()
		next.ServeHTTP(response, request)
		a.log.Info("request", "request_id", requestID, "method", request.Method,
			"path", request.URL.Path, "duration_ms", time.Since(started).Milliseconds())
	})
}

type requestIDKey struct{}

func requestID(ctx context.Context) string {
	value, _ := ctx.Value(requestIDKey{}).(string)
	return value
}

func (a *API) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		parts := strings.SplitN(request.Header.Get("Authorization"), " ", 2)
		if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
			a.fail(response, request, http.StatusUnauthorized, "unauthorized", "valid bearer token required")
			return
		}
		identity, err := a.store.Authenticate(request.Context(), parts[1])
		if err != nil {
			a.fail(response, request, http.StatusUnauthorized, "unauthorized", "device token is invalid or revoked")
			return
		}
		next.ServeHTTP(response, request.WithContext(context.WithValue(request.Context(), identityKey{}, identity)))
	})
}

func identity(ctx context.Context) store.Identity { return ctx.Value(identityKey{}).(store.Identity) }

func (a *API) health(response http.ResponseWriter, _ *http.Request) {
	writeJSON(response, http.StatusOK, map[string]string{"status": "ok"})
}

type pairRequest struct {
	PairingToken string `json:"pairing_token"`
	DisplayName  string `json:"display_name"`
	Platform     string `json:"platform"`
}

func (a *API) pair(response http.ResponseWriter, request *http.Request) {
	if !a.allowPairAttempt(request) {
		a.fail(response, request, http.StatusTooManyRequests, "pairing_rate_limited", "too many pairing attempts")
		return
	}
	var input pairRequest
	if !decodeJSON(response, request, &input, 16*1024, a.fail) {
		return
	}
	input.DisplayName = strings.TrimSpace(input.DisplayName)
	if len(input.DisplayName) < 1 || len(input.DisplayName) > 100 ||
		(input.Platform != "windows" && input.Platform != "macos") {
		a.fail(response, request, http.StatusBadRequest, "invalid_pair_request", "invalid display name or platform")
		return
	}
	hash := store.TokenHash(input.PairingToken)
	tx, err := a.store.DB.BeginTx(request.Context(), nil)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	defer tx.Rollback()
	var storedHash []byte
	var spaceID, expiry string
	var attempts int
	var consumed sql.NullString
	err = tx.QueryRowContext(request.Context(), `SELECT token_hash,space_id,expires_at,attempts_left,consumed_at
      FROM pairing_tokens WHERE token_hash=?`, hash[:]).Scan(&storedHash, &spaceID, &expiry, &attempts, &consumed)
	validHash := err == nil && len(storedHash) == len(hash) && subtle.ConstantTimeCompare(storedHash, hash[:]) == 1
	expires, parseErr := time.Parse(time.RFC3339Nano, expiry)
	if !validHash || parseErr != nil || consumed.Valid || attempts <= 0 || time.Now().UTC().After(expires) {
		if err == nil && attempts > 0 {
			_, _ = tx.ExecContext(request.Context(), "UPDATE pairing_tokens SET attempts_left=attempts_left-1 WHERE token_hash=?", hash[:])
			_ = tx.Commit()
		}
		a.fail(response, request, http.StatusUnauthorized, "invalid_pairing_token", "pairing token is invalid or expired")
		return
	}
	deviceID, _ := store.RandomHex(16)
	deviceToken, _ := store.RandomHex(32)
	deviceHash := store.TokenHash(deviceToken)
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err = tx.ExecContext(request.Context(), `INSERT INTO devices
      (id,space_id,display_name,platform,token_hash,created_at,last_seen_at) VALUES(?,?,?,?,?,?,?)`,
		deviceID, spaceID, input.DisplayName, input.Platform, deviceHash[:], now, now); err != nil {
		a.internal(response, request, err)
		return
	}
	if _, err = tx.ExecContext(request.Context(), "UPDATE pairing_tokens SET consumed_at=? WHERE token_hash=?", now, hash[:]); err != nil {
		a.internal(response, request, err)
		return
	}
	if err = tx.Commit(); err != nil {
		a.internal(response, request, err)
		return
	}
	writeJSON(response, http.StatusCreated, map[string]any{
		"device_id": deviceID, "space_id": spaceID, "device_token": deviceToken,
	})
}

func (a *API) allowPairAttempt(request *http.Request) bool {
	host, _, err := net.SplitHostPort(request.RemoteAddr)
	if err != nil {
		host = request.RemoteAddr
	}
	now := time.Now()
	a.pairMu.Lock()
	defer a.pairMu.Unlock()
	limit := a.pairLimits[host]
	if limit.reset.IsZero() || now.After(limit.reset) {
		limit = pairLimit{reset: now.Add(5 * time.Minute)}
	}
	limit.count++
	a.pairLimits[host] = limit
	return limit.count <= 30
}

func (a *API) me(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	limits, err := a.store.Limits(request.Context(), id.SpaceID)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"device_id": id.DeviceID, "space_id": id.SpaceID, "platform": id.Platform,
		"quota_bytes": limits.QuotaBytes, "max_file_bytes": limits.MaxFileBytes,
		"max_items": limits.MaxItems, "retention_seconds": limits.RetentionSeconds,
	})
}

func (a *API) devices(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	rows, err := a.store.DB.QueryContext(request.Context(), `SELECT id,display_name,platform,created_at,last_seen_at,revoked_at
      FROM devices WHERE space_id=? ORDER BY created_at,id`, id.SpaceID)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	defer rows.Close()
	values := make([]map[string]any, 0)
	for rows.Next() {
		var deviceID, name, platform, createdAt, lastSeenAt string
		var revokedAt sql.NullString
		if err := rows.Scan(&deviceID, &name, &platform, &createdAt, &lastSeenAt, &revokedAt); err != nil {
			a.internal(response, request, err)
			return
		}
		value := map[string]any{"id": deviceID, "display_name": name, "platform": platform,
			"created_at": createdAt, "last_seen_at": lastSeenAt, "revoked_at": nil}
		if revokedAt.Valid {
			value["revoked_at"] = revokedAt.String
		}
		values = append(values, value)
	}
	writeJSON(response, http.StatusOK, map[string]any{"devices": values})
}

func (a *API) revokeDevice(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	result, err := a.store.DB.ExecContext(request.Context(), `UPDATE devices SET revoked_at=?
      WHERE id=? AND space_id=? AND revoked_at IS NULL`, time.Now().UTC().Format(time.RFC3339Nano),
		request.PathValue("id"), id.SpaceID)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	count, _ := result.RowsAffected()
	if count == 0 {
		a.fail(response, request, http.StatusNotFound, "device_not_found", "active device was not found")
		return
	}
	response.WriteHeader(http.StatusNoContent)
}

type createFileRequest struct {
	ClientUploadID  string  `json:"client_upload_id"`
	Kind            string  `json:"kind"`
	Name            string  `json:"name"`
	ObjectFormat    string  `json:"object_format"`
	ObjectSizeBytes int64   `json:"object_size_bytes"`
	ObjectSHA256    string  `json:"object_sha256"`
	ExpandedBytes   int64   `json:"expanded_size_bytes"`
	EntryCount      int64   `json:"entry_count"`
	TreeSHA256      *string `json:"tree_sha256"`
}

var shaPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

func (a *API) createFile(response http.ResponseWriter, request *http.Request) {
	var input createFileRequest
	if !decodeJSON(response, request, &input, 64*1024, a.fail) {
		return
	}
	if message := validateCreate(input); message != "" {
		a.fail(response, request, http.StatusBadRequest, "invalid_file_metadata", message)
		return
	}
	id := identity(request.Context())
	limits, err := a.store.Limits(request.Context(), id.SpaceID)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if input.ObjectSizeBytes > limits.MaxFileBytes {
		a.fail(response, request, http.StatusRequestEntityTooLarge, "object_too_large", "object exceeds server limit")
		return
	}
	tx, err := a.store.DB.BeginTx(request.Context(), nil)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	defer tx.Rollback()
	var existing fileMetadata
	err = scanFile(tx.QueryRowContext(request.Context(), fileSelect+" WHERE upload_device_id=? AND client_upload_id=?",
		id.DeviceID, input.ClientUploadID), &existing)
	if err == nil {
		if sameCreate(existing, input) {
			writeJSON(response, http.StatusOK, existing)
		} else {
			a.fail(response, request, http.StatusConflict, "idempotency_conflict", "client_upload_id was reused with different metadata")
		}
		return
	}
	if !errors.Is(err, sql.ErrNoRows) {
		a.internal(response, request, err)
		return
	}
	var used, count int64
	if err = tx.QueryRowContext(request.Context(), `SELECT COALESCE(SUM(object_size_bytes),0),COUNT(*)
      FROM files WHERE space_id=? AND state IN ('uploading','ready')`, id.SpaceID).Scan(&used, &count); err != nil {
		a.internal(response, request, err)
		return
	}
	if count >= limits.MaxItems || input.ObjectSizeBytes > limits.QuotaBytes-used {
		a.fail(response, request, http.StatusRequestEntityTooLarge, "space_quota_exceeded", "space item or byte quota exceeded")
		return
	}
	fileID, _ := store.RandomHex(16)
	objectKey, _ := store.RandomHex(24)
	now := time.Now().UTC().Format(time.RFC3339Nano)
	_, err = tx.ExecContext(request.Context(), `INSERT INTO files
      (id,space_id,upload_device_id,kind,display_name,object_format,object_size_bytes,object_sha256,
       expanded_size_bytes,entry_count,tree_sha256,state,created_at,object_key,client_upload_id)
      VALUES(?,?,?,?,?,?,?,?,?,?,?,'uploading',?,?,?)`, fileID, id.SpaceID, id.DeviceID, input.Kind,
		input.Name, input.ObjectFormat, input.ObjectSizeBytes, input.ObjectSHA256, input.ExpandedBytes,
		input.EntryCount, input.TreeSHA256, now, objectKey, input.ClientUploadID)
	if err == nil {
		_, err = tx.ExecContext(request.Context(), `INSERT INTO uploads
        (file_id,reserved_bytes,started_at,last_activity_at) VALUES(?,?,?,?)`,
			fileID, input.ObjectSizeBytes, now, now)
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if err = tx.Commit(); err != nil {
		a.internal(response, request, err)
		return
	}
	created := fileMetadata{ID: fileID, UploadDeviceID: id.DeviceID, Kind: input.Kind,
		Name: input.Name, ObjectFormat: input.ObjectFormat, ObjectSizeBytes: input.ObjectSizeBytes,
		ObjectSHA256: input.ObjectSHA256, ExpandedSizeBytes: input.ExpandedBytes,
		EntryCount: input.EntryCount, TreeSHA256: input.TreeSHA256, State: "uploading", CreatedAt: now}
	writeJSON(response, http.StatusCreated, created)
}

func validateCreate(input createFileRequest) string {
	if len(input.ClientUploadID) < 8 || len(input.ClientUploadID) > 200 {
		return "invalid client_upload_id"
	}
	if input.Name == "" || len(input.Name) > 255 || strings.ContainsAny(input.Name, "\x00/\\") || input.Name == "." || input.Name == ".." {
		return "invalid display name"
	}
	if input.ObjectSizeBytes < 0 || input.ExpandedBytes < 0 || input.EntryCount < 0 || !shaPattern.MatchString(input.ObjectSHA256) {
		return "invalid size, entry count, or object SHA-256"
	}
	if input.Kind == "file" {
		if input.ObjectFormat != "raw" || input.ExpandedBytes != input.ObjectSizeBytes || input.EntryCount != 1 || input.TreeSHA256 != nil {
			return "regular files require raw format, matching expanded size, one entry, and no tree hash"
		}
		return ""
	}
	if input.Kind == "folder" {
		if input.ObjectFormat != "zip" || input.TreeSHA256 == nil || !shaPattern.MatchString(*input.TreeSHA256) ||
			input.ExpandedBytes > zipcheck.DefaultMaxExpandedBytes || input.EntryCount > zipcheck.DefaultMaxEntries {
			return "folders require zip format and valid bounded expanded metadata/tree hash"
		}
		return ""
	}
	return "kind must be file or folder"
}

func sameCreate(existing fileMetadata, input createFileRequest) bool {
	if existing.Kind != input.Kind || existing.Name != input.Name || existing.ObjectFormat != input.ObjectFormat ||
		existing.ObjectSizeBytes != input.ObjectSizeBytes || existing.ObjectSHA256 != input.ObjectSHA256 ||
		existing.ExpandedSizeBytes != input.ExpandedBytes || existing.EntryCount != input.EntryCount {
		return false
	}
	if existing.TreeSHA256 == nil || input.TreeSHA256 == nil {
		return existing.TreeSHA256 == nil && input.TreeSHA256 == nil
	}
	return *existing.TreeSHA256 == *input.TreeSHA256
}

func (a *API) putContent(response http.ResponseWriter, request *http.Request) {
	fileID := request.PathValue("id")
	a.uploadMu.Lock()
	if _, busy := a.uploading[fileID]; busy {
		a.uploadMu.Unlock()
		a.fail(response, request, http.StatusConflict, "upload_in_progress", "another PUT is active")
		return
	}
	a.uploading[fileID] = struct{}{}
	a.uploadMu.Unlock()
	defer func() { a.uploadMu.Lock(); delete(a.uploading, fileID); a.uploadMu.Unlock() }()

	id := identity(request.Context())
	var metadata fileMetadata
	var objectKey string
	err := scanFileAndKey(a.store.DB.QueryRowContext(request.Context(), fileSelectWithKey+`
      WHERE id=? AND space_id=? AND upload_device_id=?`, fileID, id.SpaceID, id.DeviceID), &metadata, &objectKey)
	if errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusNotFound, "file_not_found", "upload was not found")
		return
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if metadata.State != "uploading" {
		a.fail(response, request, http.StatusConflict, "invalid_file_state", "file is not accepting content")
		return
	}
	if request.Header.Get("Content-Type") != "application/octet-stream" {
		a.fail(response, request, http.StatusUnsupportedMediaType, "invalid_content_type", "application/octet-stream required")
		return
	}
	tempKey, _ := store.RandomHex(24)
	tempPath := a.store.UploadPath(tempKey)
	output, err := os.OpenFile(tempPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	digest := sha256.New()
	limited := io.LimitReader(request.Body, metadata.ObjectSizeBytes+1)
	written, copyErr := io.Copy(io.MultiWriter(output, digest), limited)
	syncErr := output.Sync()
	closeErr := output.Close()
	if copyErr != nil || syncErr != nil || closeErr != nil || written != metadata.ObjectSizeBytes ||
		hex.EncodeToString(digest.Sum(nil)) != metadata.ObjectSHA256 {
		_ = os.Remove(tempPath)
		a.fail(response, request, http.StatusUnprocessableEntity, "content_mismatch", "body length or SHA-256 did not match upload intent")
		return
	}
	var manifestJSON []byte
	if metadata.Kind == "folder" {
		var validated zipcheck.Result
		validated, err = zipcheck.Validate(tempPath, zipcheck.Expected{
			ExpandedBytes: metadata.ExpandedSizeBytes, EntryCount: metadata.EntryCount, TreeSHA256: *metadata.TreeSHA256,
		}, zipcheck.DefaultLimits())
		if err != nil {
			_ = os.Remove(tempPath)
			a.fail(response, request, http.StatusUnprocessableEntity, "unsafe_folder_zip", err.Error())
			return
		}
		manifestJSON, _ = json.Marshal(validated.Entries)
	}
	objectPath := a.store.ObjectPath(objectKey)
	if err = os.Rename(tempPath, objectPath); err != nil {
		_ = os.Remove(tempPath)
		a.internal(response, request, err)
		return
	}
	if dir, openErr := os.Open(filepath.Dir(objectPath)); openErr == nil {
		_ = dir.Sync()
		_ = dir.Close()
	}
	limits, _ := a.store.Limits(request.Context(), id.SpaceID)
	now := time.Now().UTC()
	readyAt := now.Format(time.RFC3339Nano)
	expiresAt := now.Add(time.Duration(limits.RetentionSeconds) * time.Second).Format(time.RFC3339Nano)
	metadata.State, metadata.ReadyAt, metadata.ExpiresAt = "ready", &readyAt, &expiresAt
	payload, _ := json.Marshal(metadata)
	tx, err := a.store.DB.BeginTx(request.Context(), nil)
	if err == nil {
		result, updateErr := tx.ExecContext(request.Context(), `UPDATE files SET state='ready',ready_at=?,expires_at=?
        WHERE id=? AND state='uploading'`, readyAt, expiresAt, fileID)
		if updateErr == nil {
			var affected int64
			affected, updateErr = result.RowsAffected()
			if affected != 1 && updateErr == nil {
				updateErr = errors.New("upload state changed")
			}
		}
		if updateErr == nil {
			_, updateErr = tx.ExecContext(request.Context(), "DELETE FROM uploads WHERE file_id=?", fileID)
		}
		if updateErr == nil && metadata.Kind == "folder" {
			_, updateErr = tx.ExecContext(request.Context(),
				"INSERT OR REPLACE INTO folder_manifests(file_id,manifest_json) VALUES(?,?)", fileID, string(manifestJSON))
		}
		var eventResult sql.Result
		if updateErr == nil {
			eventResult, updateErr = tx.ExecContext(request.Context(), `INSERT INTO events
          (space_id,type,file_id,payload_json,created_at) VALUES(?,'file.ready',?,?,?)`,
				id.SpaceID, fileID, string(payload), readyAt)
		}
		if updateErr == nil {
			seq, _ := eventResult.LastInsertId()
			_, updateErr = tx.ExecContext(request.Context(), "UPDATE event_streams SET last_seq=? WHERE space_id=?", seq, id.SpaceID)
		}
		if updateErr == nil {
			err = tx.Commit()
		} else {
			err = updateErr
			_ = tx.Rollback()
		}
	}
	if err != nil {
		a.log.Error("object committed but database publish failed; recovery required", "file_id", fileID, "error", err)
		a.internal(response, request, err)
		return
	}
	writeJSON(response, http.StatusOK, metadata)
}

func (a *API) getManifest(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	var metadata fileMetadata
	var objectKey string
	err := scanFileAndKey(a.store.DB.QueryRowContext(request.Context(), fileSelectWithKey+`
      WHERE id=? AND space_id=?`, request.PathValue("id"), id.SpaceID), &metadata, &objectKey)
	if errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusNotFound, "file_not_found", "file was not found")
		return
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if metadata.State != "ready" || metadata.Kind != "folder" {
		a.fail(response, request, http.StatusConflict, "manifest_unavailable", "ready folder required")
		return
	}
	var raw string
	err = a.store.DB.QueryRowContext(request.Context(),
		"SELECT manifest_json FROM folder_manifests WHERE file_id=?", metadata.ID).Scan(&raw)
	if errors.Is(err, sql.ErrNoRows) {
		validated, validateErr := zipcheck.Validate(a.store.ObjectPath(objectKey), zipcheck.Expected{
			ExpandedBytes: metadata.ExpandedSizeBytes, EntryCount: metadata.EntryCount, TreeSHA256: *metadata.TreeSHA256,
		}, zipcheck.DefaultLimits())
		if validateErr != nil {
			a.internal(response, request, validateErr)
			return
		}
		bytes, _ := json.Marshal(validated.Entries)
		raw = string(bytes)
		_, _ = a.store.DB.ExecContext(request.Context(),
			"INSERT OR REPLACE INTO folder_manifests(file_id,manifest_json) VALUES(?,?)", metadata.ID, raw)
	} else if err != nil {
		a.internal(response, request, err)
		return
	}
	var entries []zipcheck.Entry
	if json.Unmarshal([]byte(raw), &entries) != nil {
		a.internal(response, request, errors.New("stored manifest is invalid"))
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"entries": entries})
}

func (a *API) getContent(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	var metadata fileMetadata
	var objectKey string
	err := scanFileAndKey(a.store.DB.QueryRowContext(request.Context(), fileSelectWithKey+`
      WHERE id=? AND space_id=?`, request.PathValue("id"), id.SpaceID), &metadata, &objectKey)
	if errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusNotFound, "file_not_found", "file was not found")
		return
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if metadata.State != "ready" {
		status := http.StatusGone
		if metadata.State == "uploading" {
			status = http.StatusConflict
		}
		a.fail(response, request, status, "file_not_ready", "file content is unavailable")
		return
	}
	file, err := os.Open(a.store.ObjectPath(objectKey))
	if err != nil {
		a.log.Error("ready object missing", "file_id", metadata.ID, "error", err)
		a.fail(response, request, http.StatusInternalServerError, "object_damaged", "stored object is unavailable")
		return
	}
	defer file.Close()
	response.Header().Set("Content-Type", "application/octet-stream")
	response.Header().Set("Content-Length", strconv.FormatInt(metadata.ObjectSizeBytes, 10))
	response.Header().Set("ETag", `"sha256:`+metadata.ObjectSHA256+`"`)
	response.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": metadata.Name}))
	response.WriteHeader(http.StatusOK)
	_, _ = io.CopyN(response, file, metadata.ObjectSizeBytes)
}

func (a *API) deleteFile(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	fileID := request.PathValue("id")
	tx, err := a.store.DB.BeginTx(request.Context(), nil)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	defer tx.Rollback()
	var state string
	err = tx.QueryRowContext(request.Context(), "SELECT state FROM files WHERE id=? AND space_id=?", fileID, id.SpaceID).Scan(&state)
	if errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusNotFound, "file_not_found", "file was not found")
		return
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	if state == "deleted" || state == "expired" {
		response.WriteHeader(http.StatusNoContent)
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err = tx.ExecContext(request.Context(), "UPDATE files SET state='deleted',deleted_at=? WHERE id=?", now, fileID); err != nil {
		a.internal(response, request, err)
		return
	}
	eventResult, err := tx.ExecContext(request.Context(), `INSERT INTO events
    (space_id,type,file_id,payload_json,created_at) VALUES(?,'file.deleted',?,? ,?)`,
		id.SpaceID, fileID, `{"id":"`+fileID+`","state":"deleted"}`, now)
	if err == nil {
		seq, _ := eventResult.LastInsertId()
		_, err = tx.ExecContext(request.Context(), "UPDATE event_streams SET last_seq=? WHERE space_id=?", seq, id.SpaceID)
	}
	if err == nil {
		err = tx.Commit()
	}
	if err != nil {
		a.internal(response, request, err)
		return
	}
	response.WriteHeader(http.StatusNoContent)
}

func (a *API) snapshot(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	tx, err := a.store.DB.BeginTx(request.Context(), &sql.TxOptions{ReadOnly: true})
	if err != nil {
		a.internal(response, request, err)
		return
	}
	defer tx.Rollback()
	rows, err := tx.QueryContext(request.Context(), fileSelect+" WHERE space_id=? AND state='ready' ORDER BY created_at,id", id.SpaceID)
	if err != nil {
		a.internal(response, request, err)
		return
	}
	files := make([]fileMetadata, 0)
	for rows.Next() {
		var metadata fileMetadata
		if err = scanFile(rows, &metadata); err != nil {
			rows.Close()
			a.internal(response, request, err)
			return
		}
		files = append(files, metadata)
	}
	if err = rows.Close(); err != nil {
		a.internal(response, request, err)
		return
	}
	var seq int64
	if err = tx.QueryRowContext(request.Context(), "SELECT last_seq FROM event_streams WHERE space_id=?", id.SpaceID).Scan(&seq); err != nil {
		a.internal(response, request, err)
		return
	}
	if err = tx.Commit(); err != nil {
		a.internal(response, request, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"files": files, "snapshot_seq": seq})
}

func (a *API) events(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	after, err := strconv.ParseInt(request.URL.Query().Get("after"), 10, 64)
	if err != nil || after < 0 {
		a.fail(response, request, http.StatusBadRequest, "invalid_cursor", "after must be a non-negative integer")
		return
	}
	var last, minimum int64
	if err = a.store.DB.QueryRowContext(request.Context(), "SELECT last_seq,minimum_valid_after FROM event_streams WHERE space_id=?", id.SpaceID).Scan(&last, &minimum); err != nil {
		a.internal(response, request, err)
		return
	}
	if after < minimum || after > last {
		a.fail(response, request, http.StatusConflict, "resync_required", "event cursor is outside retained history")
		return
	}
	flusher, ok := response.(http.Flusher)
	if !ok {
		a.fail(response, request, http.StatusInternalServerError, "sse_unsupported", "streaming is unavailable")
		return
	}
	response.Header().Set("Content-Type", "text/event-stream")
	response.Header().Set("Cache-Control", "no-cache, no-transform")
	response.Header().Set("X-Accel-Buffering", "no")
	ticker := time.NewTicker(time.Second)
	heartbeat := time.NewTicker(15 * time.Second)
	defer ticker.Stop()
	defer heartbeat.Stop()
	cursor := after
	for {
		select {
		case <-request.Context().Done():
			return
		case <-heartbeat.C:
			_, _ = io.WriteString(response, ": heartbeat\n\n")
			flusher.Flush()
		case <-ticker.C:
			rows, queryErr := a.store.DB.QueryContext(request.Context(), `SELECT seq,type,payload_json
          FROM events WHERE space_id=? AND seq>? ORDER BY seq LIMIT 100`, id.SpaceID, cursor)
			if queryErr != nil {
				return
			}
			for rows.Next() {
				var seq int64
				var eventType, payload string
				if rows.Scan(&seq, &eventType, &payload) != nil {
					rows.Close()
					return
				}
				fmt.Fprintf(response, "id: %d\nevent: %s\ndata: %s\n\n", seq, eventType, payload)
				cursor = seq
			}
			rows.Close()
			flusher.Flush()
		}
	}
}

const fileSelect = `SELECT id,upload_device_id,kind,display_name,object_format,object_size_bytes,
 object_sha256,expanded_size_bytes,entry_count,tree_sha256,state,created_at,ready_at,expires_at FROM files`

const fileSelectWithKey = `SELECT id,upload_device_id,kind,display_name,object_format,object_size_bytes,
 object_sha256,expanded_size_bytes,entry_count,tree_sha256,state,created_at,ready_at,expires_at,object_key FROM files`

type scanner interface{ Scan(...any) error }

func scanFile(row scanner, output *fileMetadata) error {
	return row.Scan(&output.ID, &output.UploadDeviceID, &output.Kind, &output.Name, &output.ObjectFormat,
		&output.ObjectSizeBytes, &output.ObjectSHA256, &output.ExpandedSizeBytes, &output.EntryCount,
		&output.TreeSHA256, &output.State, &output.CreatedAt, &output.ReadyAt, &output.ExpiresAt)
}

func scanFileAndKey(row scanner, output *fileMetadata, objectKey *string) error {
	return row.Scan(&output.ID, &output.UploadDeviceID, &output.Kind, &output.Name, &output.ObjectFormat,
		&output.ObjectSizeBytes, &output.ObjectSHA256, &output.ExpandedSizeBytes, &output.EntryCount,
		&output.TreeSHA256, &output.State, &output.CreatedAt, &output.ReadyAt, &output.ExpiresAt, objectKey)
}

func decodeJSON(response http.ResponseWriter, request *http.Request, target any, max int64,
	fail func(http.ResponseWriter, *http.Request, int, string, string)) bool {
	request.Body = http.MaxBytesReader(response, request.Body, max)
	decoder := json.NewDecoder(request.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		fail(response, request, http.StatusBadRequest, "invalid_json", "request body is invalid")
		return false
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		fail(response, request, http.StatusBadRequest, "invalid_json", "request body must contain one JSON value")
		return false
	}
	return true
}

func writeJSON(response http.ResponseWriter, status int, value any) {
	response.Header().Set("Content-Type", "application/json")
	response.WriteHeader(status)
	_ = json.NewEncoder(response).Encode(value)
}

func (a *API) fail(response http.ResponseWriter, request *http.Request, status int, code, message string) {
	writeJSON(response, status, errorBody{Code: code, Message: message, RequestID: requestID(request.Context())})
}

func (a *API) internal(response http.ResponseWriter, request *http.Request, err error) {
	a.log.Error("request failed", "request_id", requestID(request.Context()), "error", err)
	a.fail(response, request, http.StatusInternalServerError, "internal_error", "request could not be completed")
}
