# DropSpace macOS clipboard v1 checklist

## Credentials and transport

- [ ] Pair with `platform: macos`; store token in Keychain.
- [ ] Use the paired base URL and bearer auth for every clipboard HTTP request and WebSocket handshake.
- [ ] Require trusted TLS; never disable certificate validation.
- [ ] Never log tokens, text, image bytes, or text previews.

## Native pasteboard

- [ ] Observe `NSPasteboard.general.changeCount` efficiently and stop when disabled.
- [ ] Support UTF-8 plain text and normalized PNG only.
- [ ] Enforce 1 MiB text, 16 MiB PNG, and safe decoded dimensions before upload/apply.
- [ ] Write a private marker with version, clipboard ID, and hash in the same native write.
- [ ] Prove a marked remote write is not uploaded again; do not use timeout-only suppression.
- [ ] Do not simulate Cmd+V.

## Protocol and state

- [ ] Send multipart metadata/content exactly as `CLIPBOARD_API.md` specifies.
- [ ] Verify downloaded size and SHA-256 before native write.
- [ ] Preserve `copied_at DESC, server_sequence DESC, clipboard_id DESC` order.
- [ ] Ignore auto-apply for the local `source_device_id`.
- [ ] Persist `last_applied_sequence` only after a successful native write.
- [ ] Treat created WebSocket messages as invalidations and refetch authoritative history.
- [ ] Recover from the initial snapshot after every reconnect; back off to at most about 30 seconds.
- [ ] Close WebSocket and stop all intervention when sharing is disabled.

## UI

- [ ] Settings include enable, status, online count, history entry, clear, and shortcut.
- [ ] History has at most 20 rows, latest first, text preview/image thumbnail, source, and copy time.
- [ ] Up/Down, Enter, Esc, click/double-click, one-item delete, and confirmed clear work.
- [ ] Selecting history writes the pasteboard only; the user pastes normally.

## Tests before merge

- [ ] Mac-to-Windows and Windows-to-Mac text, Unicode, multiline, and empty-boundary cases.
- [ ] Mac-to-Windows and Windows-to-Mac PNG, thumbnail, and size-limit cases.
- [ ] No echo loop for at least 20 consecutive bidirectional copy operations.
- [ ] Two devices copying near-simultaneously obey authoritative ordering.
- [ ] Offline copy/reconnect snapshot and missed-notification recovery.
- [ ] Disabled state does not upload or modify the local pasteboard.
- [ ] Delete/clear and blob cleanup behavior match the server.
- [ ] Revoked/invalid credentials cannot read history, content, thumbnails, or WebSocket.
- [ ] Existing macOS file drag/drop and `NSFilePromiseProvider` regression suite still passes.

