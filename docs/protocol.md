# DropSpace phase-one protocol

All production requests use HTTPS and the `/v1` prefix. Development HTTP is allowed only on localhost. Except for pairing and `GET /healthz`, requests use `Authorization: Bearer <device_token>`. The server derives the device and space from that token; clients never choose `upload_device_id` or an object storage key.

Errors use:

```json
{"code":"machine_code","message":"safe explanation","request_id":"trace id"}
```

## Pairing

An operator runs `dropspace-server pair`. The command stores only a SHA-256 token digest and prints the high-entropy token once. A client redeems it with:

```http
POST /v1/pair
Content-Type: application/json

{"pairing_token":"...","display_name":"Work PC","platform":"windows"}
```

The response contains `device_id`, `space_id`, and a one-time-visible device token. Windows clients must store the token in Credential Manager, not SQLite or logs.

## Immutable object upload

Create an intent with `POST /v1/files`. A regular file uses `kind=file`, `object_format=raw`, expanded size equal to object size, `entry_count=1`, and no tree hash. A folder uses `kind=folder`, `object_format=zip`, the ZIP byte length/hash, actual expanded byte count, ZIP entry count, and a tree hash.

`client_upload_id` is the idempotency key scoped to the authenticated device. Repeating identical metadata returns the same logical file; changing metadata returns 409.

Upload the exact bytes using `PUT /v1/files/{id}/content` and `Content-Type: application/octet-stream`. A failed, short, long, or hash-mismatching body never becomes ready and may be retried from byte zero. Range and multipart resume are deliberately absent.

The folder tree digest is SHA-256 over records sorted by canonical NFC relative path. A directory record is `D NUL path NUL`. A file record is `F NUL path NUL decimal_size NUL lowercase_content_sha256 NUL`. ZIP paths use UTF-8 and `/`; the ZIP contains the root folder's children, not an extra copy of the root name.

## Snapshot, events, and content

- `GET /v1/snapshot` returns `{files, snapshot_seq}` from one SQLite read transaction.
- `GET /v1/events?after=N` replays persisted SSE events and then waits. Query `after` is authoritative. A cursor below `minimum_valid_after` or above the durable stream head receives `resync_required`.
- `GET /v1/files/{id}/content` returns a complete immutable body, exact `Content-Length`, and `ETag: "sha256:<digest>"`.
- `GET /v1/files/{id}/manifest` returns the server-validated `{path,directory,size}` list for a ready folder ZIP. Windows uses this metadata to freeze `FILEGROUPDESCRIPTORW` before content is requested; it does not replace ZIP validation during delivery.
- `DELETE /v1/files/{id}` writes a tombstone and event before physical cleanup. It never removes a user's original or delivered copy.

Times are server-generated UTC RFC 3339 values. Sizes are integer bytes. Clients must not infer that event sequence numbers for one space are contiguous.

