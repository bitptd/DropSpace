# DropSpace clipboard data model v1

Clipboard storage is independent from file-transfer objects and from the 52,428,800-byte client precache. The migration is additive schema migration version `2` in `server/internal/store/store.go`.

## Tables

`clipboard_items` is the shared event history:

| Column | Meaning |
| --- | --- |
| `server_sequence` | SQLite autoincrement primary key and deterministic tie-breaker |
| `clipboard_id` | globally unique client event ID |
| `space_id` | owning DropSpace space |
| `source_device_id` | authenticated source device |
| `content_type` | `text` or `image/png` |
| `copied_at` | source copy time, normalized RFC3339Nano UTC |
| `content_hash` | lowercase SHA-256 of exact content bytes |
| `content_size` | exact byte length |
| `preview` | up to 240 Unicode scalar values for text; null for images |
| `object_key` | opaque server-side blob key |
| `thumbnail_key` | opaque PNG thumbnail key or null |
| `created_at` | server receipt/commit time for audit only |
| `protocol_version` | currently `1` |

The history index is `(space_id, copied_at DESC, server_sequence DESC, clipboard_id DESC)`. All API reads also constrain `space_id`.

`clipboard_blobs` owns deduplicated content:

| Column | Meaning |
| --- | --- |
| `(space_id, content_hash)` | primary key; deduplication never crosses spaces |
| `content_type`, `content_size` | immutable validation metadata |
| `object_key` | opaque unique file key |
| `thumbnail_key` | image thumbnail key or null |
| `ref_count` | number of `clipboard_items` referencing the blob |
| `created_at` | first storage time |

## Retention, ordering, and quota

After every upload, the server sorts the space by `copied_at DESC, server_sequence DESC, clipboard_id DESC`, retains the first 20, decrements blob references for the remainder, and deletes zero-reference blobs. Therefore a delayed upload can be accepted yet immediately return `retained:false`.

The independent quota is the sum of `content_size` in `clipboard_blobs`, not the sum of all history rows. The current cap is 268,435,456 bytes per space. Repeated content shares one blob and increments `ref_count`.

## Filesystem layout

With the production data directory `/var/lib/dropspace`:

```text
/var/lib/dropspace/
  dropspace.sqlite
  clipboard/
    objects/<opaque-key>.bin
    thumbnails/<opaque-key>.png
    tmp/<opaque-key>.part
```

Directories are created with mode `0700`; new objects use `0600`. Uploads enter `tmp`, are validated, then atomically renamed. Clipboard files never enter `/var/lib/dropspace/objects` and never participate in file transfer, ZIP extraction, or the client automatic cache.

## Transactions and cleanup

Metadata insertion, refcount updates, pruning, and quota evaluation run in one SQLite transaction. A new object is deleted if the transaction rolls back. After a successful delete/prune/clear transaction, unreferenced object and thumbnail files are removed. Missing files produce a controlled `410` rather than exposing a filesystem path.

Migration rollback is operational rather than automatic: restore the pre-deployment SQLite backup and prior server binary. Do not manually drop version-2 tables while a new server is running.

## Windows local state

The existing client database adds only a generic `app_settings(key, value)` table. Phase two currently stores:

- `clipboard_enabled` (`0` or `1`);
- `clipboard_hotkey` (portable Qt key-sequence text);
- `clipboard_last_applied_sequence` (highest remote sequence actually written locally).

Clipboard payloads and history are not persisted in the Windows client database. Content is fetched on demand and verified before it is written to the Windows clipboard.

