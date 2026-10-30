# DropSpace phase-one acceptance

Last updated: 2026-10-30 (Asia/Shanghai).

Legend: **PASS** was executed and observed; **IMPLEMENTED / UNVERIFIED** exists in source but lacks the named real-system test; **PENDING** is not yet implemented.

## Stage status

| Stage | Implemented | Verified on this host | Still unverified / incomplete |
| --- | --- | --- | --- |
| P0 repository and environment | CMake/Qt/Go skeleton, fixed architecture, build records and dependency pins | **PASS on Windows 11 x64:** client compiled, launched for 3 seconds, remained responsive; server compiled and `/healthz` returned `ok` | macOS intentionally handled elsewhere |
| P1 native delayed drag | Windows OLE virtual-file source, lazy `IStream`, async capability, 3 s file and 15 s nested folder-tree spike | **PASS:** automated descriptor/stream tests passed; on 2026-10-29 the user reported both packaged-client prototypes delivered correctly to Windows Explorer, including the delayed regular file and nested Unicode folder tree | Exact Windows edition/build, hashes of Explorer outputs, cancellation/failure, real ZIP extraction and localhost delayed download still need recorded evidence |
| P2 VPS minimum loop | Pairing, bearer authentication, quota reservation, idempotent intent, streamed PUT/SHA-256, ZIP validation, atomic ready publish, GET, tombstone delete, snapshot, SSE, recovery, expiry GC, verified folder-manifest API, Caddy/systemd examples | **PASS locally and on VPS:** upgraded Linux service is active; trusted HTTPS `/healthz`, authenticated upload/download, snapshot and folder-manifest requests passed | Event pruning/minimum-valid-cursor behavior, concurrent quota race and crash injection remain unverified |
| P3 upload and sync | Windows Credential Manager token storage, local SQLite identity/snapshot/original-path records, HTTPS pairing, direct file upload, folder scan/ZIP upload, two-upload cap, progress, cloud deletion, snapshot refresh and persistent SSE reconnect with backoff | **PASS on Windows over public HTTPS:** two integration runs uploaded regular files and Unicode/nested/empty folders; the latest also fetched the server-validated three-entry folder manifest | Live two-device SSE/revocation observation and a persistent retry queue remain pending |
| P4 hybrid cache and content acquisition | Exact `CachePlanner`; sequential real pre-cache downloads; `.part` + length/SHA-256 + atomic publish; corruption revalidation; distinct transfer objects; original-file snapshot/original-folder re-pack validation with VPS fallback; streaming `SafeExtractor` | **PASS:** planner exact cases plus 2,000 randomized sets; public integration deleted both original paths, then downloaded and restored the file and folder with matching bytes, Unicode path and empty directory; malicious traversal and case-collision ZIP tests passed | Durable lease/reference accounting, 4 GiB reservation ledger, cancellation, idle/startup cleanup and dynamic-plan stress remain incomplete |
| P5 integrated native delivery | Real snapshot rows connect to the Windows OLE virtual-file provider. Selection prepares and freezes verified files/folders before drag. The icon view supports rubber-band/Ctrl/Shift multi-selection, preserves the group when drag starts, resolves duplicate export names, and emits one mixed OLE descriptor set for selected files and folder trees | Automated OLE descriptor/memory-stream/file-backed-stream tests pass. The user confirmed the preflight fix resolved own-file, own-folder and cloud-folder delivery failures | Mixed multi-select implementation builds and launches but needs Explorer confirmation; cancellation and post-drag lease lifetime remain incomplete |
| P6 packaging | Release client dependency directory and Windows/Linux-amd64 server binaries generated | **PASS for final minimal unsigned Windows portable software:** the current directory contains 16 required files totaling 34,990,902 bytes. `DropSpace.exe` 1.0.6 has product/version metadata and the current white-dog icon. Native OLE drag now suspends tray outside-click dismissal for the full `DoDragDrop` lifetime, and the list receives a synthetic release/state reset afterward so rubber-band selection cannot remain armed | Tray-popup JPG/PPTX delivery and post-drag selection reset need the user's visible Explorer confirmation; no signing or installer |

## Environment record

| Item | Observed result |
| --- | --- |
| OS | Microsoft Windows 11 Pro, 10.0.26300 build 26300, x64 |
| CPU | Intel Core Ultra 7 270K Plus |
| Git | 2.56.0.windows.2 |
| Go | 1.27.0 windows/amd64; server test/build/run passed |
| CMake | 4.4.4 |
| Ninja | 1.13.2 installed, not used by the selected preset |
| Qt | 6.8.3 MinGW x64 (`qtbase`) |
| C++ compiler | MinGW-w64 GCC 13.1.0 (posix-seh) |
| macOS | Out of scope for this workspace by user direction; developed separately and never marked passed here |

## Automated acceptance matrix

| Requirement | Result | Evidence |
| --- | --- | --- |
| Empty list and all own-device objects produce no precache | **PASS** | `cache_planner_tests` |
| Current device is always excluded | **PASS** | exact and randomized tests |
| Sort by bytes, then stable tie rules | **PASS** | exact and shuffled-input property tests |
| Exactly 52,428,800 bytes fits | **PASS** | exact boundary test |
| 52,428,801-byte object does not precache | **PASS** | exact boundary test |
| 10/15/30/60 MiB selects only 10 and 15 | **PASS** | exact greedy test |
| Random plans are stable, eligible, and within limit | **PASS** | 2,000 deterministic random sets |
| OLE virtual descriptor and lazy stream contract, including file-backed stream | **PASS on Windows** | `windows_virtual_file_tests` |
| Folder scanner and ZIP object packer | **PASS on Windows** | `folder_packer_tests` |
| Safe extraction round trip, empty directory, traversal rejection, case-collision rejection and no partial publish | **PASS on Windows** | `safe_extractor_tests` |
| C++ tree digest agrees with Go ZIP validator | **PASS on Windows localhost** | generated 339-byte ZIP uploaded as folder; ready tree SHA-256 `64d19c32cea42a201839a2cefb803d21322a2f7c7e9ea83cdbc593786ebb3b83` |

Executed commands/results:

- CMake build: all client targets built with Qt 6.8.3 / GCC 13.1.0.
- CTest: 4/4 passed (`cache_planner_tests`, `windows_virtual_file_tests`, `folder_packer_tests`, `safe_extractor_tests`).
- Public Qt integration: `api_client_integration` passed regular-file and folder upload, server validation, snapshot, verified folder manifest, deliberate original-path removal, VPS fallback download and automatic safe folder restoration over `https://43.108.80.211:8443`.
- UI placement: the drop target is now one dashed container containing both its instruction and the live file list; temporary upload rows and synchronized ready rows therefore appear inside the area where the user dropped them. Debug and Release builds passed after this change.
- Explorer-style presentation: the in-zone list uses a 48 px Windows system icon for folders and each file extension, an adaptive icon grid, names below icons, and metadata/status tooltips. Upload-queue placeholders use the same system icons.
- Production UI cleanup: the main surface now contains only the dashed icon manager and a compact bottom action row with Delete and Device/Pairing. Right-clicking an item exposes Save As and confirmed cloud deletion; direct icon drag and double-click Save As remain available. P1 spike controls are hidden from the production layout.
- Blank-space context menu: Refresh requests a new server snapshot; Sort supports name, object bytes, item type/extension and upload time in ascending or descending order; View switches between the 48 px icon grid and a compact detail-style list showing name, type, bytes and local upload time.
- Window controls: the compact action row includes a checkable Always on Top button. It toggles `WindowStaysOnTopHint`, preserves the window position, highlights the active state and changes its label to Cancel Always on Top.
- Multi-selection: icon/list views use extended selection with blank-space rubber-band, Ctrl and Shift selection. All selected objects are prepared independently, dragged as one copy-only OLE operation, and deleted only after one aggregate confirmation; duplicate top-level names receive safe numbered suffixes.
- Selection visuals: list items explicitly remove `ItemIsUserCheckable` and clear `CheckStateRole`; the item-indicator style is zero-sized. Rubber-band and blue selection highlighting remain, while checkbox controls are never shown during selection or drag.
- Production packaging: the executable is `DropSpace.exe` with version 1.0.6 and a Windows resource icon generated from the current white-dog `icon.jpg`; the byte-identical release-directory JPEG is loaded directly for the Qt window/taskbar/tray icon, with an embedded copy as fallback. The minimal portable directory keeps only direct Qt/runtime dependencies plus qwindows, qsqlite, qjpeg, modern Windows style, Schannel and certificate TLS plugins.
- Tray lifecycle: QApplication remains alive after the last window closes; MainWindow converts WM_CLOSE into hide. A QSystemTrayIcon created directly from the unmodified embedded JPEG shows the same MainWindow as a 520×360 frameless tool popup positioned above the tray geometry, with only the dashed file surface visible. The tool-window implementation keeps item and blank-space context menus usable; a deferred deactivate check hides it only when focus genuinely moves outside and preserves it for menus/modal dialogs. The tray context menu separately offers Open Full Window and Full Exit.
- Go: `go test ./...` passed (`internal/api`, `internal/zipcheck`; other packages contain no tests).
- Deployed Linux amd64 server: 11,051,168 bytes, SHA-256 `9cce7633ef079b6907b0b7c80fc495fa96e3dcd474766d9ead805ec2f72455ac`; systemd returned `active` after the manifest-capable upgrade and public `/healthz` returned `{"status":"ok"}`.
- Final release: `E:\workspace\DropSpace_release`, 34,990,902 bytes across 16 files. `DropSpace.exe` is 1,310,115 bytes with SHA-256 `e8c5194842db4e8c63c974a0ff5776997c9b8c014cd05747f027bbfc77131a5d`; PE metadata reads ProductName `DropSpace`, FileDescription `DropSpace Windows Client`, FileVersion `1.0.6`. Debug and Release core suites passed 4/4 after the tray drag-lifetime and post-drag mouse-state fixes, and the packaged client remained running for the three-second dependency smoke test.

## Native Windows matrix

The first two rows are user-reported real Explorer results. They are not mislabeled as independently observed automated evidence.

| Action | Windows Explorer result |
| --- | --- |
| 3-second uncached regular virtual file | **PASS — user-reported on 2026-10-29** |
| 15-second uncached nested folder tree and empty directory | **PASS — user-reported on 2026-10-29** |
| Cached and 60 MiB uncached real objects | PENDING |
| ZIP folder automatically restored as real folder via explicit Save As | **PASS in public automated integration** |
| Real VPS-backed file/folder OLE drag button to Explorer | IMPLEMENTED / UNVERIFIED |
| Mixed multi-select and same-name conflict | PENDING |
| Cancellation, failure, disconnect, expiry, disk full | PENDING |
| Target reads after drag returns while cleanup runs | PENDING |
| Third-party targets and explicit fallback | PENDING |

