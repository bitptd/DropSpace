# DropSpace clipboard WebSocket v1

Connect to `/v1/clipboard/ws` using `wss` when the paired HTTP endpoint uses `https`. Send the same `Authorization: Bearer <device_token>` header as HTTP. Compression is disabled and messages are UTF-8 JSON metadata only; clipboard bodies are always downloaded through authenticated HTTP.

Every message contains:

```json
{"clipboard_protocol_version":1,"type":"..."}
```

Unknown message types must be ignored. A protocol version other than `1` must not be applied.

## Snapshot

The server's first message is authoritative recovery state:

```json
{
  "clipboard_protocol_version": 1,
  "type": "clipboard.snapshot",
  "online_devices": 2,
  "items": [/* the authoritative ordered history, at most 20 */]
}
```

On every connection/reconnection, replace the displayed history from this snapshot and evaluate only `items[0]` as the latest shared value. This makes reconnect correct even when transient notifications were dropped.

## Created

```json
{
  "clipboard_protocol_version": 1,
  "type": "clipboard.created",
  "item": {/* same item shape as HTTP history */}
}
```

Treat this as an invalidation notification. Fetch `/v1/clipboard/history?limit=20` before automatically changing the system clipboard, because delayed device uploads can make the newly created record older than the current `items[0]` by `copied_at`.

Do not download content when `source_device_id` is the local device. For a remote latest item, apply it only when its `server_sequence` is newer than the locally persisted last-applied sequence. Download, enforce size, verify SHA-256, then update the native clipboard.

## Delete and clear

```json
{"clipboard_protocol_version":1,"type":"clipboard.deleted","clipboard_id":"...","server_sequence":31}
```

```json
{"clipboard_protocol_version":1,"type":"clipboard.cleared"}
```

Refresh the authoritative history. Deletion does not require a client to replace the current system clipboard with an older item.

## Presence

```json
{"clipboard_protocol_version":1,"type":"clipboard.presence","online_devices":2}
```

`online_devices` counts distinct authenticated devices with at least one active clipboard WebSocket for this space. It is not the number of all paired devices. Multiple sockets from one device count once.

## Liveness and recovery

The server sends a WebSocket ping every 30 seconds. The Windows client reconnects with exponential delays from 1 second up to 30 seconds. A transient notification can be dropped for a slow subscriber; the snapshot and HTTP history are the source of truth.

Recommended client algorithm:

1. Open the WebSocket after pairing only when sharing is enabled.
2. On snapshot, display `items`, then consider `items[0]` for remote apply.
3. On created/delete/clear, fetch HTTP history and recompute from the authoritative order.
4. On disconnect, show offline/reconnecting state and use exponential backoff.
5. On reconnect, trust the new snapshot rather than replay assumptions.
6. When sharing is disabled, close the socket, stop native monitoring, and do not apply remote events.

