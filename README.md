# DropSpace

DropSpace is a Windows Qt 6 desktop client plus a single-process Go/SQLite VPS relay. Files upload directly. Folders are encoded as one immutable ZIP object and are safely restored to real folders at delivery. Version 2 adds opt-in cross-device plain-text and PNG clipboard synchronization, a 20-item history panel, authenticated WebSocket recovery, and a native Windows global shortcut. This workspace intentionally does not implement the macOS client; its Codex implementation consumes the same protocols.

Development is in progress. The authoritative implementation status and executed evidence are in [docs/acceptance.md](docs/acceptance.md).

The macOS file-transfer handoff, including the AppKit `NSFilePromiseProvider` design, is in [docs/MACOS_CODEX_HANDOFF.md](docs/MACOS_CODEX_HANDOFF.md). Clipboard protocol and native integration requirements are in [docs/CLIPBOARD_MACOS_INTEGRATION.md](docs/CLIPBOARD_MACOS_INTEGRATION.md) and [docs/CLIPBOARD_MACOS_CHECKLIST.md](docs/CLIPBOARD_MACOS_CHECKLIST.md).

## Windows build

The checked-in preset expects CMake 3.24 or newer plus Qt 6.8.3 and its MinGW 13.1 kit under `.tools/Qt` (the local verified paths are `.tools/Qt/6.8.3/mingw_64` and `.tools/Qt/Tools/mingw1310_64`):

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

The release preset is `windows-mingw-release`. The production target is named `DropSpace.exe`; version 2.0.0 loads the byte-identical `icon.jpg` beside the executable for its window/taskbar/tray icon, with the embedded copy as fallback, while the PE shell icon uses a multi-size ICO encoding of the same uncropped, unadjusted image. The current verified minimal portable delivery is `E:\workspace\DropSpace_release`. It is unsigned and does not require installation. Closing the full window hides it to the system tray; left-click shows a frameless dashed-file popup immediately above the tray icon. The Device/Pairing dialog contains the independent shared-clipboard switch, status, online count, history entry, clear action, and configurable global shortcut (default `Ctrl+Shift+V`).

## Local server

After installing Go 1.24 or newer:

```powershell
cd server
go test ./...
go run ./cmd/dropspace-server pair -config ../deploy/config.example.json
go run ./cmd/dropspace-server serve -config ../deploy/config.example.json
```

For Windows-local development, copy `deploy/config.example.json` and change `data_dir` to an absolute writable Windows directory and set `development` to `true`. Production keeps the service on loopback behind Caddy with valid HTTPS. File API details are in [docs/protocol.md](docs/protocol.md); clipboard HTTP and WebSocket details are in [docs/CLIPBOARD_API.md](docs/CLIPBOARD_API.md) and [docs/CLIPBOARD_WEBSOCKET.md](docs/CLIPBOARD_WEBSOCKET.md).

The currently deployed development VPS endpoint is `https://43.108.80.211:8443`. It uses a publicly trusted short-lived IP certificate with automated renewal; do not substitute plaintext HTTP or disable certificate validation.

To connect the Windows client, first create a ten-minute one-time token on the VPS:

```bash
sudo -u dropspace /usr/local/bin/dropspace-server pair \
  -config /etc/dropspace/config.json -minutes 10
```

Open the client, choose **设备设置 / 配对**, keep the server address `https://43.108.80.211:8443`, and enter the one-time token. The resulting device token is stored in Windows Credential Manager; SQLite contains only non-secret device identity and metadata. After pairing, dropping a regular file starts hashing/upload immediately, while dropping a folder scans it, creates the immutable ZIP staging object, uploads it, and removes the staging directory when the task finishes.

