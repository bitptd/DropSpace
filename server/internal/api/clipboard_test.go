package api

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"

	"dropspace/server/internal/store"
)

func clipboardMultipart(t *testing.T, id, contentType string, copiedAt time.Time, content []byte) ([]byte, string) {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	metadata, err := writer.CreateFormField("metadata")
	if err != nil {
		t.Fatal(err)
	}
	if err = json.NewEncoder(metadata).Encode(clipboardUploadMetadata{
		ProtocolVersion: clipboardProtocolVersion,
		ClipboardID:     id,
		ContentType:     contentType,
		CopiedAt:        copiedAt.UTC().Format(time.RFC3339Nano),
		ContentHash:     sha(content),
		ContentSize:     int64(len(content)),
	}); err != nil {
		t.Fatal(err)
	}
	headers := textproto.MIMEHeader{}
	headers.Set("Content-Disposition", `form-data; name="content"; filename="clipboard.bin"`)
	headers.Set("Content-Type", "application/octet-stream")
	part, err := writer.CreatePart(headers)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = part.Write(content); err != nil {
		t.Fatal(err)
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	return body.Bytes(), writer.FormDataContentType()
}

func uploadClipboard(t *testing.T, server *testServer, token, id, contentType string,
	copiedAt time.Time, content []byte) (clipboardItem, bool, int) {
	t.Helper()
	body, mediaType := clipboardMultipart(t, id, contentType, copiedAt, content)
	response := server.request(http.MethodPost, "/v1/clipboard", token, body, mediaType)
	defer response.Body.Close()
	var result struct {
		Item     clipboardItem `json:"item"`
		Retained bool          `json:"retained"`
	}
	if response.StatusCode == http.StatusCreated || response.StatusCode == http.StatusOK {
		if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
			t.Fatal(err)
		}
	}
	return result.Item, result.Retained, response.StatusCode
}

func TestClipboardTextLifecycleOrderingAndIdempotency(t *testing.T) {
	server := newTestServer(t)
	token := server.pair("device A")
	unauthorized := server.request(http.MethodGet, "/v1/clipboard/history", "", nil, "")
	if unauthorized.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthorized history status: %d", unauthorized.StatusCode)
	}
	unauthorized.Body.Close()

	now := time.Now().UTC()
	first, retained, status := uploadClipboard(t, server, token, "clipboard-text-0001", "text", now.Add(-2*time.Second), []byte("中文\nline two"))
	if status != http.StatusCreated || !retained || first.ContentType != "text" {
		t.Fatalf("first upload: status=%d retained=%v item=%+v", status, retained, first)
	}
	second, _, status := uploadClipboard(t, server, token, "clipboard-text-0002", "text", now.Add(-time.Second), []byte("B"))
	if status != http.StatusCreated {
		t.Fatalf("second upload status: %d", status)
	}
	third, _, status := uploadClipboard(t, server, token, "clipboard-text-0003", "text", now.Add(-time.Second), []byte("C"))
	if status != http.StatusCreated || third.ServerSequence <= second.ServerSequence {
		t.Fatalf("sequence was not increasing: second=%d third=%d", second.ServerSequence, third.ServerSequence)
	}
	duplicate, _, status := uploadClipboard(t, server, token, "clipboard-text-0001", "text", now.Add(-2*time.Second), []byte("中文\nline two"))
	if status != http.StatusOK || duplicate.ServerSequence != first.ServerSequence {
		t.Fatal("idempotent clipboard upload created a different item")
	}

	history := server.request(http.MethodGet, "/v1/clipboard/history?limit=20", token, nil, "")
	defer history.Body.Close()
	var result struct {
		Items []clipboardItem `json:"items"`
	}
	if err := json.NewDecoder(history.Body).Decode(&result); err != nil {
		t.Fatal(err)
	}
	if history.StatusCode != http.StatusOK || len(result.Items) != 3 {
		t.Fatalf("history: status=%d items=%d", history.StatusCode, len(result.Items))
	}
	if result.Items[0].ClipboardID != third.ClipboardID || result.Items[1].ClipboardID != second.ClipboardID || result.Items[2].ClipboardID != first.ClipboardID {
		t.Fatalf("history ordering was not copied_at then sequence: %+v", result.Items)
	}

	content := server.request(http.MethodGet, first.ContentReference, token, nil, "")
	contentBytes, _ := io.ReadAll(content.Body)
	content.Body.Close()
	if content.StatusCode != http.StatusOK || string(contentBytes) != "中文\nline two" {
		t.Fatal("text clipboard content round trip failed")
	}

	conflictBody, conflictType := clipboardMultipart(t, "clipboard-text-0001", "text", now.Add(-2*time.Second), []byte("different"))
	conflict := server.request(http.MethodPost, "/v1/clipboard", token, conflictBody, conflictType)
	if conflict.StatusCode != http.StatusConflict {
		t.Fatalf("clipboard id conflict status: %d", conflict.StatusCode)
	}
	conflict.Body.Close()
}

func TestClipboardSpaceIsolation(t *testing.T) {
	server := newTestServer(t)
	tokenA := server.pair("space A device")
	now := time.Now().UTC()
	item, _, status := uploadClipboard(t, server, tokenA, "clipboard-isolation-0001", "text", now, []byte("space A secret"))
	if status != http.StatusCreated {
		t.Fatalf("space A upload status: %d", status)
	}

	spaceB, err := store.RandomHex(16)
	if err != nil {
		t.Fatal(err)
	}
	_, err = server.store.DB.Exec(`INSERT INTO spaces
      (id,name,quota_bytes,max_file_bytes,max_items,retention_seconds) VALUES(?,?,?,?,?,?)`,
		spaceB, "space B", int64(1024*1024*1024), int64(1024*1024), 100, 86400)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = server.store.DB.Exec("INSERT INTO event_streams(space_id) VALUES(?)", spaceB); err != nil {
		t.Fatal(err)
	}
	pairingToken, err := server.store.CreatePairingToken(context.Background(), spaceB, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	pairResponse := server.request(http.MethodPost, "/v1/pair", "", map[string]any{
		"pairing_token": pairingToken, "display_name": "space B device", "platform": "windows",
	}, "application/json")
	var paired struct {
		DeviceToken string `json:"device_token"`
	}
	if err = json.NewDecoder(pairResponse.Body).Decode(&paired); err != nil {
		pairResponse.Body.Close()
		t.Fatal(err)
	}
	pairResponse.Body.Close()
	if pairResponse.StatusCode != http.StatusCreated {
		t.Fatalf("space B pair status: %d", pairResponse.StatusCode)
	}

	history := server.request(http.MethodGet, "/v1/clipboard/history", paired.DeviceToken, nil, "")
	var result struct {
		Items []clipboardItem `json:"items"`
	}
	if err = json.NewDecoder(history.Body).Decode(&result); err != nil {
		history.Body.Close()
		t.Fatal(err)
	}
	history.Body.Close()
	if history.StatusCode != http.StatusOK || len(result.Items) != 0 {
		t.Fatalf("space B saw space A history: status=%d items=%d", history.StatusCode, len(result.Items))
	}
	content := server.request(http.MethodGet, item.ContentReference, paired.DeviceToken, nil, "")
	content.Body.Close()
	if content.StatusCode != http.StatusNotFound {
		t.Fatalf("space B content isolation status: %d", content.StatusCode)
	}
}

func TestClipboardHistoryLimitAndClockValidation(t *testing.T) {
	server := newTestServer(t)
	token := server.pair("device A")
	now := time.Now().UTC()
	for index := 0; index < 22; index++ {
		id := fmt.Sprintf("clipboard-limit-%02d-item", index)
		_, _, status := uploadClipboard(t, server, token, id, "text", now.Add(time.Duration(index)*time.Millisecond), []byte(id))
		if status != http.StatusCreated {
			t.Fatalf("upload %d status: %d", index, status)
		}
	}
	history := server.request(http.MethodGet, "/v1/clipboard/history", token, nil, "")
	var result struct {
		Items []clipboardItem `json:"items"`
	}
	_ = json.NewDecoder(history.Body).Decode(&result)
	history.Body.Close()
	if len(result.Items) != clipboardHistoryLimit {
		t.Fatalf("history length: %d", len(result.Items))
	}
	var count int
	if err := server.store.DB.QueryRow("SELECT COUNT(*) FROM clipboard_items").Scan(&count); err != nil || count != clipboardHistoryLimit {
		t.Fatalf("database history count=%d err=%v", count, err)
	}
	body, mediaType := clipboardMultipart(t, "clipboard-clock-outlier", "text", now.Add(48*time.Hour), []byte("future"))
	outlier := server.request(http.MethodPost, "/v1/clipboard", token, body, mediaType)
	if outlier.StatusCode != http.StatusUnprocessableEntity {
		t.Fatalf("clock outlier status: %d", outlier.StatusCode)
	}
	outlier.Body.Close()
}

func TestClipboardImageThumbnailDedupAndCleanup(t *testing.T) {
	server := newTestServer(t)
	token := server.pair("device A")
	imageValue := image.NewRGBA(image.Rect(0, 0, 32, 16))
	for y := 0; y < 16; y++ {
		for x := 0; x < 32; x++ {
			imageValue.Set(x, y, color.RGBA{R: uint8(x * 7), G: uint8(y * 11), B: 80, A: 255})
		}
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, imageValue); err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	first, _, status := uploadClipboard(t, server, token, "clipboard-image-0001", "image/png", now, encoded.Bytes())
	if status != http.StatusCreated || first.Thumbnail == nil {
		t.Fatalf("image upload status=%d thumbnail=%v", status, first.Thumbnail)
	}
	second, _, status := uploadClipboard(t, server, token, "clipboard-image-0002", "image/png", now.Add(time.Millisecond), encoded.Bytes())
	if status != http.StatusCreated {
		t.Fatalf("second image status: %d", status)
	}
	var blobCount, refCount int
	if err := server.store.DB.QueryRow("SELECT COUNT(*),MAX(ref_count) FROM clipboard_blobs").Scan(&blobCount, &refCount); err != nil || blobCount != 1 || refCount != 2 {
		t.Fatalf("dedup count=%d refs=%d err=%v", blobCount, refCount, err)
	}
	thumbnail := server.request(http.MethodGet, *first.Thumbnail, token, nil, "")
	thumbnailBytes, _ := io.ReadAll(thumbnail.Body)
	thumbnail.Body.Close()
	if thumbnail.StatusCode != http.StatusOK || len(thumbnailBytes) == 0 {
		t.Fatal("thumbnail download failed")
	}
	if _, err := png.Decode(bytes.NewReader(thumbnailBytes)); err != nil {
		t.Fatal("thumbnail is not PNG:", err)
	}
	deleted := server.request(http.MethodDelete, "/v1/clipboard/"+first.ClipboardID, token, nil, "")
	deleted.Body.Close()
	if deleted.StatusCode != http.StatusOK {
		t.Fatalf("first delete status: %d", deleted.StatusCode)
	}
	if err := server.store.DB.QueryRow("SELECT ref_count FROM clipboard_blobs").Scan(&refCount); err != nil || refCount != 1 {
		t.Fatalf("remaining ref count=%d err=%v", refCount, err)
	}
	cleared := server.request(http.MethodDelete, "/v1/clipboard", token, nil, "")
	clearBody, _ := io.ReadAll(cleared.Body)
	cleared.Body.Close()
	if cleared.StatusCode != http.StatusOK {
		t.Fatalf("clear status: %d body=%s", cleared.StatusCode, clearBody)
	}
	if err := server.store.DB.QueryRow("SELECT COUNT(*) FROM clipboard_blobs").Scan(&blobCount); err != nil || blobCount != 0 {
		t.Fatalf("blob cleanup count=%d err=%v", blobCount, err)
	}
	_ = second
}

func TestClipboardWebSocketSnapshotAndPush(t *testing.T) {
	server := newTestServer(t)
	token := server.pair("device A")
	websocketURL := "ws" + strings.TrimPrefix(server.server.URL, "http") + "/v1/clipboard/ws"
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	connection, response, err := websocket.Dial(ctx, websocketURL, &websocket.DialOptions{
		HTTPHeader: http.Header{"Authorization": []string{"Bearer " + token}},
	})
	if err != nil {
		if response != nil {
			t.Fatalf("websocket dial status=%d err=%v", response.StatusCode, err)
		}
		t.Fatal(err)
	}
	defer connection.CloseNow()
	_, initial, err := connection.Read(ctx)
	if err != nil || !bytes.Contains(initial, []byte(`"type":"clipboard.snapshot"`)) {
		t.Fatalf("initial websocket snapshot: %s err=%v", initial, err)
	}
	if !bytes.Contains(initial, []byte(`"online_devices":1`)) {
		t.Fatalf("initial websocket presence: %s", initial)
	}
	_, _, status := uploadClipboard(t, server, token, "clipboard-websocket-0001", "text", time.Now().UTC(), []byte("pushed"))
	if status != http.StatusCreated {
		t.Fatalf("websocket source upload: %d", status)
	}
	var message []byte
	for attempt := 0; attempt < 3; attempt++ {
		_, message, err = connection.Read(ctx)
		if err != nil || bytes.Contains(message, []byte(`"type":"clipboard.created"`)) {
			break
		}
	}
	if err != nil || !bytes.Contains(message, []byte(`"type":"clipboard.created"`)) || !bytes.Contains(message, []byte("clipboard-websocket-0001")) {
		t.Fatalf("websocket created event: %s err=%v", message, err)
	}
}
