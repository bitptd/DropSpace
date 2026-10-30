# DropSpace architecture (phase one)

Status: active implementation, 2026-10-29.

## Supported products

- Desktop client in this repository: Windows 11 x64, C++20 and Qt 6 Widgets.
- Server: one Linux VPS process, Go standard HTTP server and SQLite.
- macOS client: Qt 6 Widgets/C++20 with a narrow Objective-C++ AppKit,
  Foundation, Security and UniformTypeIdentifiers bridge. Finder delivery uses
  `NSFilePromiseProvider`; credentials use macOS Keychain. The API and immutable
  ZIP object contract remain platform-neutral.
- No Linux desktop client, mobile client, browser runtime, Redis, object-storage SDK, file-system driver, Shell extension, or directory-sync framework.

## Fixed behavior

The VPS is authoritative. A file becomes visible only after its complete immutable object has landed and passed length and SHA-256 validation. A regular file is a raw object. A folder is scanned, rejected if it contains links/reparse points or unsafe names, and encoded as one standard ZIP object. Network upload, download, retry, validation, automatic precache, and leasing operate on that single object abstraction. Delivery is the only kind-specific layer: raw bytes become a file; a ZIP is safely expanded and atomically published as a real folder.

Automatic precache has the hard per-device limit `52,428,800` bytes. Eligible ready and unexpired objects exclude `upload_device_id == current_device_id`, sort by `(object_size_bytes, created_at, id)`, and are greedily selected. An object that does not fit is skipped. A separate transfer area has a default 4 GiB budget.

Windows delivery uses OLE virtual files (`CFSTR_FILEDESCRIPTORW`, `CFSTR_FILECONTENTS`, `IStream`, and `IDataObjectAsyncCapability`). It does not require or attempt to discover the Explorer target directory. Existing local export paths may use `CF_HDROP` only after safe preparation and with a lease.

## Selected versions and environment

The development host observed on 2026-10-29 is Windows 11 Pro x64, version 10.0.26300 (build 26300), Intel Core Ultra 7 270K Plus. The exercised toolchain is Git 2.56.0.windows.2, Go 1.27.0, CMake 4.4.4, Qt 6.8.3, and MinGW-w64 GCC 13.1.0. Ninja 1.13.2 is installed but the reproducible preset uses MinGW Makefiles.

The repository pins Qt 6.8.3 in its Windows CMake preset and `modernc.org/sqlite` 1.39.1 in `go.mod`. The folder packer currently uses Qt's version-pinned private `QZipWriter`; this keeps ZIP creation streaming and dependency-light, but it must be retested or replaced when upgrading Qt because it is not a stable public API.

The macOS development host observed on 2026-10-30 is macOS 26.5.2 on an
Apple M3 (`arm64`), with Xcode 26.6, Apple Clang 21.0.0, CMake 4.1.1,
Ninja 1.11.1 and Homebrew Qt 6.12.0. The `macos-arm64-debug` preset pins the
Homebrew `qtbase` prefix and arm64 architecture. Qt 6.12.0 private ZIP headers
were exercised by the folder packer and extractor tests; this does not claim
Intel macOS coverage.

## Module boundaries

- `client/src/core`: I/O-free policies such as `planPrecache`, then content acquisition, leases, and safe extraction.
- `client/src/storage`: local SQLite metadata. Original absolute paths and credentials never sync to the VPS.
- `client/src/platform/windows`: narrow OLE bridge. It consumes frozen descriptors and content callbacks; it never edits the cache database.
- `client/src/platform/macos`: narrow AppKit File Promise and Security.framework
  Keychain bridges. Promise work runs on a background operation queue and holds
  providers, delegates, task contexts and content leases through completion.
- `server/internal`: authentication, SQLite store, API, persistent events, ZIP validation, recovery, and garbage collection.

The content service will expose a leased `acquireReadableObject(file_id, reason)` result. It tries a validated snapshot of an own-device original first, then a verified precache/transfer object, then a verified full VPS download. No mutable source path is handed directly to a target.

## Default limits

| Limit | Value |
| --- | ---: |
| Automatic precache | 52,428,800 bytes |
| Immutable object | 1 GiB |
| Folder expanded bytes | 4 GiB |
| Folder entries | 10,000 |
| Folder depth | 64 |
| Space quota including upload reservations | 10 GiB |
| Ready entries | 10,000 |
| VPS retention after ready | 24 hours |
| Client transfer area | 4 GiB |
| Upload/foreground download/precache concurrency | 2 / 2 / 1 |
| Event retention | 7 days |

