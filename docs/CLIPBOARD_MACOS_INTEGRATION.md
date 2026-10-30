# DropSpace macOS clipboard integration guide

The VPS clipboard protocol is implemented and versioned as `clipboard_protocol_version: 1`. macOS should add a native bridge and reuse the existing device authentication. The canonical HTTP shapes are in `CLIPBOARD_API.md`; WebSocket behavior is in `CLIPBOARD_WEBSOCKET.md`.

## 1. Register and authenticate the device

Use `POST /v1/pair` with `platform: "macos"`, a display name, and the user's one-time pairing token. Persist `device_id`, `space_id`, server base URL, and the returned `device_token`; put the token in Keychain, never UserDefaults or logs. All subsequent HTTP and WebSocket requests send `Authorization: Bearer <device_token>`.

## 2. Native interface to implement

Create an isolated macOS adapter, for example `MacClipboardBridge`, with these behaviors:

- start/stop observing `NSPasteboard.general.changeCount` without polling aggressively;
- read plain text as UTF-8 bytes;
- read supported images and normalize them to complete PNG bytes;
- write verified UTF-8 text or PNG to `NSPasteboard.general`;
- write/read a private DropSpace pasteboard type containing protocol version, clipboard ID, and content hash;
- emit a local-copy event containing type, bytes, and UTC copy time;
- never simulate Cmd+V or keyboard input.

Use native AppKit APIs. The file-promise drag implementation is unrelated and should remain unchanged.

## 3. Upload text

When sharing is enabled and a new local pasteboard change is supported:

1. Ignore it if the private DropSpace marker is present and valid.
2. Convert the exact string to UTF-8; ignore/report values over 1,048,576 bytes.
3. Generate a new UUID clipboard ID and RFC3339 UTC `copied_at`.
4. Compute lowercase SHA-256 of the exact UTF-8 bytes.
5. Send the multipart request documented in `CLIPBOARD_API.md` with `content_type: "text"`.

## 4. Upload images

Choose an AppKit-supported image representation, decode it with bounded dimensions, and encode a normalized PNG. Reject/report PNG data over 16,777,216 bytes or unsafe dimensions. Hash and size the final PNG bytes—not the source TIFF/HEIC representation—and upload with `content_type: "image/png"`.

## 5. Fetch and display the last 20 records

Call `GET /v1/clipboard/history?limit=20`. Preserve the returned order. Show text `preview`; for images load the authenticated relative `thumbnail` URL. Show `source_device_name` and localize `copied_at` for display. Do not use `created_at` for sorting.

The native global history shortcut is a macOS product choice; once invoked, Up/Down selects, Enter downloads/applies, and Esc closes. Selecting an entry writes it to the pasteboard and does not paste into another application.

## 6. Receive WebSocket updates

Open `wss://<paired-host>/v1/clipboard/ws` with the bearer header only while sharing is enabled. Process snapshot, created, deleted, cleared, and presence messages exactly as documented. On `clipboard.created`, refetch HTTP history before applying, because the created item may not be latest by `copied_at`.

## 7. Download content

Resolve `content_reference` against the paired base URL, authenticate, stream with a 16 MiB hard ceiling, and compare the final byte count and SHA-256 to metadata. Text must also be valid UTF-8; images must decode as safe PNG. Never write unverified bytes to `NSPasteboard`.

Use `thumbnail` only for UI. To apply an image, always download `content_reference`.

## 8. Update the system clipboard

For verified remote text, declare the private DropSpace marker and the standard string type in one pasteboard update. For PNG, declare the marker plus PNG and suitable standard image representations. The marker must include at least protocol version, clipboard ID, and content hash. Update a persisted `last_applied_sequence` only after the native write succeeds.

## 9. Prevent synchronization loops

Do not rely on a timeout. A remote write must carry the private marker. The observer reads the marker on the resulting `changeCount` and suppresses upload. Keep source-device filtering as an additional guard: never auto-apply an item whose `source_device_id` equals the local device ID.

## 10. copied_at and server_sequence

`copied_at` is the source event time. Generate it in UTC with subsecond precision. The server accepts at most ±24 hours from its own clock. History order is `copied_at DESC, server_sequence DESC, clipboard_id DESC`.

`server_sequence` is a deterministic server tie-breaker and local apply cursor, not a replacement for `copied_at`. Persist the highest sequence actually written to the local pasteboard. Always use the server-provided ordered first item when deciding the latest value.

## 11. Offline recovery

Reconnect with exponential backoff capped at roughly 30 seconds. Every successful WebSocket connection begins with `clipboard.snapshot`; replace local in-memory history from it. HTTP history is authoritative, so correctness must not depend on receiving every transient WebSocket notification.

## 12. Shared-clipboard switch

When disabled:

- stop native observation;
- close WebSocket and cancel reconnect;
- do not upload, download automatically, or modify the pasteboard;
- keep cloud history intact.

When re-enabled, restart observation, connect, consume the snapshot, and apply the current remote latest only if it passes source/sequence checks. Persist the preference locally.

## 13. Delete and clear history

Use `DELETE /v1/clipboard/{id}` for one record and `DELETE /v1/clipboard` for all shared history after explicit user confirmation. Refresh history after either operation. Do not clear the macOS system pasteboard simply because cloud history was deleted.

## 14. Required macOS components

- Keychain-backed device credential store (existing pairing integration may be reused).
- `MacClipboardBridge` for observation, bounded text/image conversion, private marker, and native write.
- Clipboard HTTP client for upload/history/latest/content/thumbnail/delete/clear.
- Authenticated WebSocket client with reconnect and snapshot recovery.
- Clipboard controller implementing enable state, source filtering, hash/size verification, apply cursor, and loop prevention.
- Compact 20-item history UI with thumbnails, device/time metadata, keyboard navigation, and clear/delete actions.
- Settings UI for enable state, status, online-device count, history entry, and shortcut.
- Unit/integration tests for text, PNG, marker suppression, ordering, reconnect recovery, disabled state, limits, and deletion.

## Known v1 limits

Only text and PNG are synchronized. Rich text, HTML, files, URLs as special types, video, and arbitrary pasteboard formats are ignored. Payloads are protected in transit by TLS and at rest by VPS filesystem permissions, but v1 is not end-to-end encrypted from the VPS. Device clocks more than 24 hours wrong are rejected.

