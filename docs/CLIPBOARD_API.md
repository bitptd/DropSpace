# DropSpace clipboard HTTP API v1

This document describes the implemented API. The protocol version is `1`. The public base URL currently used by clients is `https://43.108.80.211:8443`; clients must store the base URL from pairing and must not hard-code it.

## Authentication and common rules

Except for `/healthz` and `/v1/pair`, every request uses the device credential returned by pairing:

```http
Authorization: Bearer <device_token>
```

The token identifies both the device and its space. Every clipboard query is restricted to that space. Error bodies are JSON:

```json
{"code":"unauthorized","message":"valid bearer token required","request_id":"..."}
```

Clipboard payloads are not written to normal request logs. The server logs the method, path, request ID, and duration.

Implemented limits:

- types: UTF-8 `text` and `image/png` only;
- text: at most 1,048,576 bytes;
- PNG: at most 16,777,216 bytes, 16,384 pixels per side, and 40,000,000 decoded pixels;
- history: exactly the newest 20 records according to the ordering below;
- unique blob quota: 268,435,456 bytes per space;
- accepted device-clock difference: at most 24 hours from server time;
- image thumbnail: PNG with longest side at most 256 pixels.

## Pairing

`POST /v1/pair` is the existing pairing endpoint. A macOS request uses:

```json
{
  "pairing_token": "one-time-token",
  "display_name": "Lyu's Mac",
  "platform": "macos"
}
```

The response includes `device_id`, `space_id`, and `device_token`. Store the token in Keychain and send it as the bearer token thereafter. Pairing tokens are one-time, expire, have a bounded attempt count, and pairing is rate limited.

## Upload content

`POST /v1/clipboard` uses `multipart/form-data` with exactly these logical parts:

- `metadata`: `application/json`;
- `content`: raw UTF-8 bytes or complete PNG bytes.

Metadata example:

```json
{
  "clipboard_protocol_version": 1,
  "clipboard_id": "9e122b8c-49fc-4336-a77d-f36e2bc5097b",
  "content_type": "text",
  "copied_at": "2026-10-30T08:12:34.123Z",
  "content_hash": "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
  "content_size": 11
}
```

`content_hash` is lowercase SHA-256 of the exact content bytes. `content_size` is the exact byte count. `copied_at` is RFC 3339 with a timezone and is normalized by the server to UTC. Generate a new UUID-style `clipboard_id` for every local copy event.

Success is `201 Created`. Repeating the same ID, device, metadata, and bytes is idempotent and returns `200 OK`. Reusing the ID with different data returns `409 clipboard_id_conflict`.

```json
{
  "clipboard_protocol_version": 1,
  "retained": true,
  "item": {
    "clipboard_id": "9e122b8c-49fc-4336-a77d-f36e2bc5097b",
    "source_device_id": "...",
    "source_device_name": "Windows PC",
    "content_type": "text",
    "copied_at": "2026-10-30T08:12:34.123Z",
    "server_sequence": 31,
    "content_hash": "...",
    "content_size": 11,
    "preview": "hello world",
    "content_reference": "/v1/clipboard/9e122b8c-49fc-4336-a77d-f36e2bc5097b/content",
    "thumbnail": null,
    "created_at": "2026-10-30T08:12:34.300Z",
    "clipboard_protocol_version": 1
  }
}
```

`retained` can be `false` when the uploaded event is already outside the newest 20 by the authoritative ordering. A PNG item has `preview: null` and a non-null `thumbnail` reference.

## Read history and latest

`GET /v1/clipboard/history?limit=20` returns `{clipboard_protocol_version, items}`. Values below 1 or above 20 are normalized to 20.

`GET /v1/clipboard/latest` returns `{clipboard_protocol_version, item}` or `404 clipboard_empty`.

The only authoritative order is:

```sql
copied_at DESC, server_sequence DESC, clipboard_id DESC
```

`copied_at` represents the source-device copy event. `server_sequence` is an increasing server-side tie-breaker and event cursor; `created_at` is audit information and never determines history order.

## Download content and thumbnail

Use the relative references in the item with the same authenticated base URL:

- `GET /v1/clipboard/{clipboard_id}/content`
- `GET /v1/clipboard/{clipboard_id}/thumbnail`

Content is `text/plain; charset=utf-8` or `image/png`. Thumbnails are `image/png`. Responses set `Cache-Control: private, no-store`. A client must enforce the declared size limit and verify both `content_size` and SHA-256 before writing to the system clipboard. Do not treat a thumbnail as the original image.

## Delete

- `DELETE /v1/clipboard/{clipboard_id}` deletes one history record. Repeating it succeeds with `{"deleted":false}`.
- `DELETE /v1/clipboard` clears the space's shared clipboard history and returns `{"cleared":true}`.

Referenced blob files and thumbnails are removed when their reference count reaches zero. Clear/delete events are also sent over the WebSocket.

## Common status codes

| Status | Meaning |
| --- | --- |
| 200/201 | success / created |
| 400 | invalid multipart or metadata syntax |
| 401 | missing, invalid, or revoked device token |
| 404 | item/history content not found |
| 409 | clipboard ID conflict |
| 410 | metadata exists but object is missing |
| 413 | request/content too large |
| 422 | invalid type, hash, size, UTF-8, PNG, timestamp, or protocol |
| 429 | pairing rate limit (pairing only) |
| 507 | independent clipboard blob quota exceeded |

