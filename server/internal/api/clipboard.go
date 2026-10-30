package api

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/png"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/coder/websocket"

	"dropspace/server/internal/store"
)

const (
	clipboardProtocolVersion = 1
	clipboardHistoryLimit    = 20
	clipboardMaxTextBytes    = int64(1024 * 1024)
	clipboardMaxImageBytes   = int64(16 * 1024 * 1024)
	clipboardQuotaBytes      = int64(256 * 1024 * 1024)
	clipboardClockTolerance  = 24 * time.Hour
)

var clipboardIDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9-]{15,79}$`)

type clipboardUploadMetadata struct {
	ProtocolVersion int    `json:"clipboard_protocol_version"`
	ClipboardID     string `json:"clipboard_id"`
	ContentType     string `json:"content_type"`
	CopiedAt        string `json:"copied_at"`
	ContentHash     string `json:"content_hash"`
	ContentSize     int64  `json:"content_size"`
	Preview         string `json:"-"`
}

type clipboardItem struct {
	ClipboardID       string  `json:"clipboard_id"`
	SourceDeviceID    string  `json:"source_device_id"`
	SourceDeviceName  string  `json:"source_device_name"`
	ContentType       string  `json:"content_type"`
	CopiedAt          string  `json:"copied_at"`
	ServerSequence    int64   `json:"server_sequence"`
	ContentHash       string  `json:"content_hash"`
	ContentSize       int64   `json:"content_size"`
	Preview           *string `json:"preview"`
	ContentReference  string  `json:"content_reference"`
	Thumbnail         *string `json:"thumbnail"`
	CreatedAt         string  `json:"created_at"`
	ProtocolVersion   int     `json:"clipboard_protocol_version"`
	internalObjectKey string
	internalThumbKey  sql.NullString
}

type clipboardHub struct {
	mu          sync.Mutex
	subscribers map[string]map[chan []byte]struct{}
	devices     map[string]map[string]int
}

func newClipboardHub() *clipboardHub {
	return &clipboardHub{
		subscribers: make(map[string]map[chan []byte]struct{}),
		devices:     make(map[string]map[string]int),
	}
}

func (h *clipboardHub) subscribe(spaceID, deviceID string) (<-chan []byte, int, func()) {
	channel := make(chan []byte, 32)
	h.mu.Lock()
	if h.subscribers[spaceID] == nil {
		h.subscribers[spaceID] = make(map[chan []byte]struct{})
	}
	if h.devices[spaceID] == nil {
		h.devices[spaceID] = make(map[string]int)
	}
	h.subscribers[spaceID][channel] = struct{}{}
	h.devices[spaceID][deviceID]++
	online := len(h.devices[spaceID])
	h.mu.Unlock()
	h.publishPresence(spaceID)
	return channel, online, func() {
		h.mu.Lock()
		delete(h.subscribers[spaceID], channel)
		h.devices[spaceID][deviceID]--
		if h.devices[spaceID][deviceID] <= 0 {
			delete(h.devices[spaceID], deviceID)
		}
		if len(h.subscribers[spaceID]) == 0 {
			delete(h.subscribers, spaceID)
		}
		if len(h.devices[spaceID]) == 0 {
			delete(h.devices, spaceID)
		}
		h.mu.Unlock()
		h.publishPresence(spaceID)
	}
}

func (h *clipboardHub) publishPresence(spaceID string) {
	h.mu.Lock()
	online := len(h.devices[spaceID])
	h.mu.Unlock()
	h.publish(spaceID, map[string]any{
		"clipboard_protocol_version": clipboardProtocolVersion,
		"type":                       "clipboard.presence",
		"online_devices":             online,
	})
}

func (h *clipboardHub) publish(spaceID string, message any) {
	encoded, err := json.Marshal(message)
	if err != nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for channel := range h.subscribers[spaceID] {
		select {
		case channel <- encoded:
		default:
			// A reconnect always refreshes the authoritative 20-item history.
			// Dropping a transient notification is safer than blocking uploads.
		}
	}
}

func (a *API) createClipboard(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	request.Body = http.MaxBytesReader(response, request.Body, clipboardMaxImageBytes+128*1024)
	reader, err := request.MultipartReader()
	if err != nil {
		a.fail(response, request, http.StatusBadRequest, "invalid_clipboard_multipart", "multipart metadata and content are required")
		return
	}
	tempKey, err := store.RandomHex(16)
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot allocate clipboard storage")
		return
	}
	tempPath := a.store.ClipboardTempPath(tempKey)
	defer os.Remove(tempPath)
	var metadataBytes []byte
	var contentSeen bool
	var contentSize int64
	hash := sha256.New()
	for {
		part, nextErr := reader.NextPart()
		if errors.Is(nextErr, io.EOF) {
			break
		}
		if nextErr != nil {
			a.fail(response, request, http.StatusBadRequest, "invalid_clipboard_multipart", "cannot read multipart body")
			return
		}
		switch part.FormName() {
		case "metadata":
			if metadataBytes != nil {
				part.Close()
				a.fail(response, request, http.StatusBadRequest, "duplicate_clipboard_part", "metadata part must appear once")
				return
			}
			metadataBytes, err = io.ReadAll(io.LimitReader(part, 64*1024+1))
			if err != nil || len(metadataBytes) > 64*1024 {
				part.Close()
				a.fail(response, request, http.StatusBadRequest, "invalid_clipboard_metadata", "clipboard metadata is too large")
				return
			}
		case "content":
			if contentSeen {
				part.Close()
				a.fail(response, request, http.StatusBadRequest, "duplicate_clipboard_part", "content part must appear once")
				return
			}
			contentSeen = true
			file, createErr := os.OpenFile(tempPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
			if createErr != nil {
				part.Close()
				a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot create clipboard staging file")
				return
			}
			contentSize, err = io.Copy(io.MultiWriter(file, hash), io.LimitReader(part, clipboardMaxImageBytes+1))
			closeErr := file.Close()
			if err != nil || closeErr != nil || contentSize > clipboardMaxImageBytes {
				part.Close()
				a.fail(response, request, http.StatusRequestEntityTooLarge, "clipboard_content_too_large", "clipboard content exceeds the configured limit")
				return
			}
		}
		part.Close()
	}
	if metadataBytes == nil || !contentSeen {
		a.fail(response, request, http.StatusBadRequest, "missing_clipboard_part", "metadata and content parts are required")
		return
	}
	var input clipboardUploadMetadata
	decoder := json.NewDecoder(strings.NewReader(string(metadataBytes)))
	decoder.DisallowUnknownFields()
	if err = decoder.Decode(&input); err != nil {
		a.fail(response, request, http.StatusBadRequest, "invalid_clipboard_metadata", "clipboard metadata is invalid")
		return
	}
	copiedAt, validationCode, validationMessage := validateClipboardUpload(input, contentSize, hex.EncodeToString(hash.Sum(nil)))
	if validationCode != "" {
		a.fail(response, request, http.StatusUnprocessableEntity, validationCode, validationMessage)
		return
	}
	input.CopiedAt = copiedAt.Format(time.RFC3339Nano)
	input.ContentHash = strings.ToLower(input.ContentHash)
	thumbnailTemp := ""
	if input.ContentType == "text" {
		if contentSize > clipboardMaxTextBytes {
			a.fail(response, request, http.StatusRequestEntityTooLarge, "clipboard_text_too_large", "text clipboard content exceeds 1 MiB")
			return
		}
		bytes, readErr := os.ReadFile(tempPath)
		if readErr != nil || !utf8.Valid(bytes) {
			a.fail(response, request, http.StatusUnprocessableEntity, "invalid_clipboard_text", "text clipboard content must be valid UTF-8")
			return
		}
		preview := []rune(string(bytes))
		if len(preview) > 240 {
			preview = preview[:240]
		}
		input.Preview = string(preview)
	} else {
		thumbnailTemp = a.store.ClipboardTempPath(tempKey + "-thumbnail")
		defer os.Remove(thumbnailTemp)
		if err = makeClipboardThumbnail(tempPath, thumbnailTemp); err != nil {
			a.fail(response, request, http.StatusUnprocessableEntity, "invalid_clipboard_image", "image clipboard content must be a safe PNG")
			return
		}
	}
	item, retained, duplicate, err := a.commitClipboard(request.Context(), id, input, tempPath, thumbnailTemp)
	if err != nil {
		if errors.Is(err, errClipboardConflict) {
			a.fail(response, request, http.StatusConflict, "clipboard_id_conflict", "clipboard_id already exists with different content")
			return
		}
		if errors.Is(err, errClipboardQuota) {
			a.fail(response, request, http.StatusInsufficientStorage, "clipboard_quota_exceeded", "clipboard storage quota is exhausted")
			return
		}
		a.log.Error("clipboard commit failed", "request_id", requestID(request.Context()), "error", err)
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot store clipboard content")
		return
	}
	status := http.StatusCreated
	if duplicate {
		status = http.StatusOK
	}
	writeJSON(response, status, map[string]any{
		"clipboard_protocol_version": clipboardProtocolVersion,
		"retained":                   retained,
		"item":                       item,
	})
	if retained && !duplicate {
		a.clipboard.publish(id.SpaceID, map[string]any{
			"clipboard_protocol_version": clipboardProtocolVersion,
			"type":                       "clipboard.created",
			"item":                       item,
		})
	}
}

func validateClipboardUpload(input clipboardUploadMetadata, actualSize int64, actualHash string) (time.Time, string, string) {
	if input.ProtocolVersion != clipboardProtocolVersion {
		return time.Time{}, "unsupported_clipboard_protocol", "clipboard_protocol_version must be 1"
	}
	if !clipboardIDPattern.MatchString(input.ClipboardID) {
		return time.Time{}, "invalid_clipboard_id", "clipboard_id has an invalid format"
	}
	if input.ContentType != "text" && input.ContentType != "image/png" {
		return time.Time{}, "unsupported_clipboard_type", "content_type must be text or image/png"
	}
	if input.ContentSize != actualSize || input.ContentSize < 0 {
		return time.Time{}, "clipboard_size_mismatch", "declared clipboard size does not match content"
	}
	if len(input.ContentHash) != 64 || strings.ToLower(input.ContentHash) != actualHash {
		return time.Time{}, "clipboard_hash_mismatch", "declared SHA-256 does not match content"
	}
	copiedAt, err := time.Parse(time.RFC3339Nano, input.CopiedAt)
	if err != nil {
		return time.Time{}, "invalid_copied_at", "copied_at must be UTC RFC 3339"
	}
	copiedAt = copiedAt.UTC()
	now := time.Now().UTC()
	if copiedAt.Before(now.Add(-clipboardClockTolerance)) || copiedAt.After(now.Add(clipboardClockTolerance)) {
		return time.Time{}, "copied_at_out_of_range", "device clock differs from the server by more than 24 hours"
	}
	return copiedAt, "", ""
}

var (
	errClipboardConflict = errors.New("clipboard id conflict")
	errClipboardQuota    = errors.New("clipboard quota exceeded")
)

func (a *API) commitClipboard(ctx context.Context, id store.Identity, input clipboardUploadMetadata,
	tempPath, thumbnailTemp string) (clipboardItem, bool, bool, error) {
	cleanupNewFiles := false
	newObjectKey := ""
	newThumbnailKey := ""
	defer func() {
		if cleanupNewFiles {
			os.Remove(a.store.ClipboardObjectPath(newObjectKey))
			if newThumbnailKey != "" {
				os.Remove(a.store.ClipboardThumbnailPath(newThumbnailKey))
			}
		}
	}()
	var existing clipboardItem
	err := scanClipboardItem(a.store.DB.QueryRowContext(ctx, clipboardSelect+` WHERE i.clipboard_id=? AND i.space_id=?`,
		input.ClipboardID, id.SpaceID), &existing)
	if err == nil {
		if existing.SourceDeviceID != id.DeviceID || existing.ContentHash != input.ContentHash ||
			existing.ContentType != input.ContentType || existing.ContentSize != input.ContentSize ||
			existing.CopiedAt != input.CopiedAt {
			return clipboardItem{}, false, false, errClipboardConflict
		}
		return existing, true, true, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return clipboardItem{}, false, false, err
	}

	tx, err := a.store.DB.BeginTx(ctx, nil)
	if err != nil {
		return clipboardItem{}, false, false, err
	}
	defer tx.Rollback()
	createdAt := time.Now().UTC().Format(time.RFC3339Nano)
	var objectKey, thumbnailKey string
	var storedType string
	var storedSize int64
	err = tx.QueryRowContext(ctx, `SELECT content_type,content_size,object_key,COALESCE(thumbnail_key,'')
      FROM clipboard_blobs WHERE space_id=? AND content_hash=?`, id.SpaceID, input.ContentHash).
		Scan(&storedType, &storedSize, &objectKey, &thumbnailKey)
	if errors.Is(err, sql.ErrNoRows) {
		objectKey, err = store.RandomHex(24)
		if err != nil {
			return clipboardItem{}, false, false, err
		}
		if input.ContentType == "image/png" {
			thumbnailKey, err = store.RandomHex(24)
			if err != nil {
				return clipboardItem{}, false, false, err
			}
		}
		if err = os.Rename(tempPath, a.store.ClipboardObjectPath(objectKey)); err != nil {
			return clipboardItem{}, false, false, err
		}
		if thumbnailKey != "" {
			if err = os.Rename(thumbnailTemp, a.store.ClipboardThumbnailPath(thumbnailKey)); err != nil {
				os.Remove(a.store.ClipboardObjectPath(objectKey))
				return clipboardItem{}, false, false, err
			}
		}
		cleanupNewFiles = true
		newObjectKey = objectKey
		newThumbnailKey = thumbnailKey
		_, err = tx.ExecContext(ctx, `INSERT INTO clipboard_blobs
          (space_id,content_hash,content_type,content_size,object_key,thumbnail_key,ref_count,created_at)
          VALUES(?,?,?,?,?,?,1,?)`, id.SpaceID, input.ContentHash, input.ContentType,
			input.ContentSize, objectKey, nullIfEmpty(thumbnailKey), createdAt)
		if err != nil {
			return clipboardItem{}, false, false, err
		}
	} else if err != nil {
		return clipboardItem{}, false, false, err
	} else {
		if storedType != input.ContentType || storedSize != input.ContentSize {
			return clipboardItem{}, false, false, errClipboardConflict
		}
		if _, err = tx.ExecContext(ctx, `UPDATE clipboard_blobs SET ref_count=ref_count+1
          WHERE space_id=? AND content_hash=?`, id.SpaceID, input.ContentHash); err != nil {
			return clipboardItem{}, false, false, err
		}
	}

	result, err := tx.ExecContext(ctx, `INSERT INTO clipboard_items
      (clipboard_id,space_id,source_device_id,content_type,copied_at,content_hash,content_size,
       preview,object_key,thumbnail_key,created_at,protocol_version) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`,
		input.ClipboardID, id.SpaceID, id.DeviceID, input.ContentType, input.CopiedAt,
		input.ContentHash, input.ContentSize, nullIfEmpty(input.Preview), objectKey, nullIfEmpty(thumbnailKey), createdAt,
		clipboardProtocolVersion)
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "unique") {
			return clipboardItem{}, false, false, errClipboardConflict
		}
		return clipboardItem{}, false, false, err
	}
	sequence, err := result.LastInsertId()
	if err != nil {
		return clipboardItem{}, false, false, err
	}

	removedObjects, retained, err := pruneClipboardHistory(ctx, tx, id.SpaceID, input.ClipboardID)
	if err != nil {
		return clipboardItem{}, false, false, err
	}
	var quota int64
	if err = tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(content_size),0) FROM clipboard_blobs WHERE space_id=?`, id.SpaceID).Scan(&quota); err != nil {
		return clipboardItem{}, false, false, err
	}
	if quota > clipboardQuotaBytes {
		return clipboardItem{}, false, false, errClipboardQuota
	}
	if err = tx.Commit(); err != nil {
		return clipboardItem{}, false, false, err
	}
	cleanupNewFiles = false
	for _, object := range removedObjects {
		os.Remove(a.store.ClipboardObjectPath(object.objectKey))
		if object.thumbnailKey != "" {
			os.Remove(a.store.ClipboardThumbnailPath(object.thumbnailKey))
		}
	}
	item := clipboardItem{
		ClipboardID: input.ClipboardID, SourceDeviceID: id.DeviceID,
		ContentType: input.ContentType, CopiedAt: input.CopiedAt, ServerSequence: sequence,
		ContentHash: input.ContentHash, ContentSize: input.ContentSize, CreatedAt: createdAt,
		ProtocolVersion: clipboardProtocolVersion, internalObjectKey: objectKey,
	}
	_ = scanClipboardItem(a.store.DB.QueryRowContext(ctx, clipboardSelect+` WHERE i.clipboard_id=? AND i.space_id=?`,
		input.ClipboardID, id.SpaceID), &item)
	return item, retained, false, nil
}

type removedClipboardObject struct{ objectKey, thumbnailKey string }

func pruneClipboardHistory(ctx context.Context, tx *sql.Tx, spaceID, insertedID string) ([]removedClipboardObject, bool, error) {
	rows, err := tx.QueryContext(ctx, `SELECT clipboard_id,content_hash FROM clipboard_items
      WHERE space_id=? ORDER BY copied_at DESC,server_sequence DESC,clipboard_id DESC LIMIT -1 OFFSET ?`,
		spaceID, clipboardHistoryLimit)
	if err != nil {
		return nil, false, err
	}
	type stale struct{ id, hash string }
	var staleItems []stale
	for rows.Next() {
		var value stale
		if err = rows.Scan(&value.id, &value.hash); err != nil {
			rows.Close()
			return nil, false, err
		}
		staleItems = append(staleItems, value)
	}
	rows.Close()
	retained := true
	for _, value := range staleItems {
		if value.id == insertedID {
			retained = false
		}
		if _, err = tx.ExecContext(ctx, "DELETE FROM clipboard_items WHERE clipboard_id=? AND space_id=?", value.id, spaceID); err != nil {
			return nil, false, err
		}
		if _, err = tx.ExecContext(ctx, `UPDATE clipboard_blobs SET ref_count=ref_count-1
          WHERE space_id=? AND content_hash=?`, spaceID, value.hash); err != nil {
			return nil, false, err
		}
	}
	rows, err = tx.QueryContext(ctx, `SELECT object_key,COALESCE(thumbnail_key,'') FROM clipboard_blobs
      WHERE space_id=? AND ref_count<=0`, spaceID)
	if err != nil {
		return nil, false, err
	}
	var removed []removedClipboardObject
	for rows.Next() {
		var value removedClipboardObject
		if err = rows.Scan(&value.objectKey, &value.thumbnailKey); err != nil {
			rows.Close()
			return nil, false, err
		}
		removed = append(removed, value)
	}
	rows.Close()
	if _, err = tx.ExecContext(ctx, "DELETE FROM clipboard_blobs WHERE space_id=? AND ref_count<=0", spaceID); err != nil {
		return nil, false, err
	}
	return removed, retained, nil
}

func nullIfEmpty(value string) any {
	if value == "" {
		return nil
	}
	return value
}

const clipboardSelect = `SELECT i.clipboard_id,i.source_device_id,d.display_name,i.content_type,
  i.copied_at,i.server_sequence,i.content_hash,i.content_size,i.preview,i.object_key,i.thumbnail_key,
  i.created_at,i.protocol_version FROM clipboard_items i
  JOIN devices d ON d.id=i.source_device_id`

type rowScanner interface{ Scan(...any) error }

func scanClipboardItem(row rowScanner, item *clipboardItem) error {
	err := row.Scan(&item.ClipboardID, &item.SourceDeviceID, &item.SourceDeviceName,
		&item.ContentType, &item.CopiedAt, &item.ServerSequence, &item.ContentHash,
		&item.ContentSize, &item.Preview, &item.internalObjectKey, &item.internalThumbKey,
		&item.CreatedAt, &item.ProtocolVersion)
	if err != nil {
		return err
	}
	item.ContentReference = "/v1/clipboard/" + item.ClipboardID + "/content"
	if item.internalThumbKey.Valid {
		thumbnail := "/v1/clipboard/" + item.ClipboardID + "/thumbnail"
		item.Thumbnail = &thumbnail
	} else {
		item.Thumbnail = nil
	}
	return nil
}

func (a *API) loadClipboardHistory(ctx context.Context, spaceID string, limit int) ([]clipboardItem, error) {
	if limit < 1 || limit > clipboardHistoryLimit {
		limit = clipboardHistoryLimit
	}
	rows, err := a.store.DB.QueryContext(ctx, clipboardSelect+`
      WHERE i.space_id=? ORDER BY i.copied_at DESC,i.server_sequence DESC,i.clipboard_id DESC LIMIT ?`,
		spaceID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]clipboardItem, 0, limit)
	for rows.Next() {
		var item clipboardItem
		if err = scanClipboardItem(rows, &item); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (a *API) clipboardHistory(response http.ResponseWriter, request *http.Request) {
	limit, _ := strconv.Atoi(request.URL.Query().Get("limit"))
	items, err := a.loadClipboardHistory(request.Context(), identity(request.Context()).SpaceID, limit)
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot read clipboard history")
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"clipboard_protocol_version": clipboardProtocolVersion, "items": items})
}

func (a *API) clipboardLatest(response http.ResponseWriter, request *http.Request) {
	items, err := a.loadClipboardHistory(request.Context(), identity(request.Context()).SpaceID, 1)
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot read latest clipboard item")
		return
	}
	if len(items) == 0 {
		a.fail(response, request, http.StatusNotFound, "clipboard_empty", "clipboard history is empty")
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"clipboard_protocol_version": clipboardProtocolVersion, "item": items[0]})
}

func (a *API) clipboardContent(response http.ResponseWriter, request *http.Request) {
	a.serveClipboardObject(response, request, false)
}

func (a *API) clipboardThumbnail(response http.ResponseWriter, request *http.Request) {
	a.serveClipboardObject(response, request, true)
}

func (a *API) serveClipboardObject(response http.ResponseWriter, request *http.Request, thumbnail bool) {
	id := identity(request.Context())
	var item clipboardItem
	err := scanClipboardItem(a.store.DB.QueryRowContext(request.Context(), clipboardSelect+`
      WHERE i.clipboard_id=? AND i.space_id=?`, request.PathValue("id"), id.SpaceID), &item)
	if errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusNotFound, "clipboard_not_found", "clipboard item does not exist")
		return
	}
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot read clipboard item")
		return
	}
	path := a.store.ClipboardObjectPath(item.internalObjectKey)
	contentType := "text/plain; charset=utf-8"
	if item.ContentType == "image/png" {
		contentType = "image/png"
	}
	if thumbnail {
		if !item.internalThumbKey.Valid {
			a.fail(response, request, http.StatusNotFound, "thumbnail_not_available", "clipboard item has no thumbnail")
			return
		}
		path = a.store.ClipboardThumbnailPath(item.internalThumbKey.String)
		contentType = "image/png"
	}
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		a.fail(response, request, http.StatusGone, "clipboard_content_missing", "clipboard content is no longer available")
		return
	}
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot open clipboard content")
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot inspect clipboard content")
		return
	}
	response.Header().Set("Content-Type", contentType)
	response.Header().Set("Content-Length", strconv.FormatInt(info.Size(), 10))
	response.Header().Set("Cache-Control", "private, no-store")
	response.Header().Set("ETag", `"sha256:`+item.ContentHash+`"`)
	response.WriteHeader(http.StatusOK)
	_, _ = io.Copy(response, file)
}

func (a *API) deleteClipboard(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	item, removed, err := a.removeClipboardItems(request.Context(), id.SpaceID, request.PathValue("id"))
	if errors.Is(err, sql.ErrNoRows) {
		writeJSON(response, http.StatusOK, map[string]any{"deleted": false})
		return
	}
	if err != nil {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot delete clipboard item")
		return
	}
	a.removeClipboardFiles(removed)
	writeJSON(response, http.StatusOK, map[string]any{"deleted": true, "clipboard_id": item.ClipboardID})
	a.clipboard.publish(id.SpaceID, map[string]any{
		"clipboard_protocol_version": clipboardProtocolVersion,
		"type":                       "clipboard.deleted",
		"clipboard_id":               item.ClipboardID,
		"server_sequence":            item.ServerSequence,
	})
}

func (a *API) clearClipboard(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	_, removed, err := a.removeClipboardItems(request.Context(), id.SpaceID, "")
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		a.fail(response, request, http.StatusInternalServerError, "storage_error", "cannot clear clipboard history")
		return
	}
	a.removeClipboardFiles(removed)
	writeJSON(response, http.StatusOK, map[string]any{"cleared": true})
	a.clipboard.publish(id.SpaceID, map[string]any{
		"clipboard_protocol_version": clipboardProtocolVersion,
		"type":                       "clipboard.cleared",
	})
}

func (a *API) removeClipboardItems(ctx context.Context, spaceID, clipboardID string) (clipboardItem, []removedClipboardObject, error) {
	tx, err := a.store.DB.BeginTx(ctx, nil)
	if err != nil {
		return clipboardItem{}, nil, err
	}
	defer tx.Rollback()
	var item clipboardItem
	if clipboardID != "" {
		err = scanClipboardItem(tx.QueryRowContext(ctx, clipboardSelect+` WHERE i.clipboard_id=? AND i.space_id=?`, clipboardID, spaceID), &item)
		if err != nil {
			return clipboardItem{}, nil, err
		}
	}
	rows, err := tx.QueryContext(ctx, `SELECT content_hash FROM clipboard_items
      WHERE space_id=? AND (?='' OR clipboard_id=?)`, spaceID, clipboardID, clipboardID)
	if err != nil {
		return clipboardItem{}, nil, err
	}
	var hashes []string
	for rows.Next() {
		var hash string
		if err = rows.Scan(&hash); err != nil {
			rows.Close()
			return clipboardItem{}, nil, err
		}
		hashes = append(hashes, hash)
	}
	rows.Close()
	if len(hashes) == 0 {
		return clipboardItem{}, nil, sql.ErrNoRows
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM clipboard_items
      WHERE space_id=? AND (?='' OR clipboard_id=?)`, spaceID, clipboardID, clipboardID); err != nil {
		return clipboardItem{}, nil, err
	}
	for _, hash := range hashes {
		if _, err = tx.ExecContext(ctx, `UPDATE clipboard_blobs SET ref_count=ref_count-1
          WHERE space_id=? AND content_hash=?`, spaceID, hash); err != nil {
			return clipboardItem{}, nil, err
		}
	}
	rows, err = tx.QueryContext(ctx, `SELECT object_key,COALESCE(thumbnail_key,'') FROM clipboard_blobs
      WHERE space_id=? AND ref_count<=0`, spaceID)
	if err != nil {
		return clipboardItem{}, nil, err
	}
	var removed []removedClipboardObject
	for rows.Next() {
		var value removedClipboardObject
		if err = rows.Scan(&value.objectKey, &value.thumbnailKey); err != nil {
			rows.Close()
			return clipboardItem{}, nil, err
		}
		removed = append(removed, value)
	}
	rows.Close()
	if _, err = tx.ExecContext(ctx, "DELETE FROM clipboard_blobs WHERE space_id=? AND ref_count<=0", spaceID); err != nil {
		return clipboardItem{}, nil, err
	}
	return item, removed, tx.Commit()
}

func (a *API) removeClipboardFiles(objects []removedClipboardObject) {
	for _, object := range objects {
		if err := os.Remove(a.store.ClipboardObjectPath(object.objectKey)); err != nil && !errors.Is(err, os.ErrNotExist) {
			a.log.Error("remove clipboard object failed", "object_key", object.objectKey, "error", err)
		}
		if object.thumbnailKey != "" {
			if err := os.Remove(a.store.ClipboardThumbnailPath(object.thumbnailKey)); err != nil && !errors.Is(err, os.ErrNotExist) {
				a.log.Error("remove clipboard thumbnail failed", "thumbnail_key", object.thumbnailKey, "error", err)
			}
		}
	}
}

func (a *API) clipboardWebSocket(response http.ResponseWriter, request *http.Request) {
	id := identity(request.Context())
	connection, err := websocket.Accept(response, request, &websocket.AcceptOptions{
		CompressionMode: websocket.CompressionDisabled,
	})
	if err != nil {
		return
	}
	defer connection.CloseNow()
	connection.SetReadLimit(4096)
	ctx, cancel := context.WithCancel(request.Context())
	defer cancel()
	go func() {
		for {
			if _, _, readErr := connection.Read(ctx); readErr != nil {
				cancel()
				return
			}
		}
	}()
	messages, onlineDevices, unsubscribe := a.clipboard.subscribe(id.SpaceID, id.DeviceID)
	defer unsubscribe()
	items, err := a.loadClipboardHistory(ctx, id.SpaceID, clipboardHistoryLimit)
	if err != nil {
		_ = connection.Close(websocket.StatusInternalError, "history unavailable")
		return
	}
	initial, _ := json.Marshal(map[string]any{
		"clipboard_protocol_version": clipboardProtocolVersion,
		"type":                       "clipboard.snapshot",
		"items":                      items,
		"online_devices":             onlineDevices,
	})
	if err = connection.Write(ctx, websocket.MessageText, initial); err != nil {
		return
	}
	ping := time.NewTicker(30 * time.Second)
	defer ping.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case message := <-messages:
			writeCtx, stop := context.WithTimeout(ctx, 10*time.Second)
			err = connection.Write(writeCtx, websocket.MessageText, message)
			stop()
			if err != nil {
				return
			}
		case <-ping.C:
			pingCtx, stop := context.WithTimeout(ctx, 10*time.Second)
			err = connection.Ping(pingCtx)
			stop()
			if err != nil {
				return
			}
		}
	}
}

func makeClipboardThumbnail(source, destination string) error {
	file, err := os.Open(source)
	if err != nil {
		return err
	}
	config, err := png.DecodeConfig(file)
	file.Close()
	if err != nil || config.Width < 1 || config.Height < 1 || config.Width > 16384 || config.Height > 16384 ||
		int64(config.Width)*int64(config.Height) > 40_000_000 {
		return errors.New("unsafe PNG dimensions")
	}
	file, err = os.Open(source)
	if err != nil {
		return err
	}
	decoded, err := png.Decode(file)
	file.Close()
	if err != nil {
		return err
	}
	bounds := decoded.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	const maximum = 256
	thumbWidth, thumbHeight := width, height
	if width > maximum || height > maximum {
		if width >= height {
			thumbWidth = maximum
			thumbHeight = max(1, height*maximum/width)
		} else {
			thumbHeight = maximum
			thumbWidth = max(1, width*maximum/height)
		}
	}
	thumbnail := image.NewRGBA(image.Rect(0, 0, thumbWidth, thumbHeight))
	for y := 0; y < thumbHeight; y++ {
		for x := 0; x < thumbWidth; x++ {
			sourceX := bounds.Min.X + x*width/thumbWidth
			sourceY := bounds.Min.Y + y*height/thumbHeight
			thumbnail.Set(x, y, decoded.At(sourceX, sourceY))
		}
	}
	if err = os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		return err
	}
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	encodeErr := png.Encode(output, thumbnail)
	closeErr := output.Close()
	if encodeErr != nil {
		return encodeErr
	}
	return closeErr
}

func (item clipboardItem) String() string {
	return fmt.Sprintf("clipboard %s sequence %d", item.ClipboardID, item.ServerSequence)
}
