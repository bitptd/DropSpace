# DropSpace phase-one acceptance

Last updated: 2026-10-30 (Asia/Shanghai).

Legend: **PASS** was executed and observed; **IMPLEMENTED / UNVERIFIED** exists in source but lacks the named real-system test; **PENDING** is not yet implemented.

## Stage status

| Stage | Implemented | Verified on this host | Still unverified / incomplete |
| --- | --- | --- | --- |
| P0 repository and environment | CMake/Qt/Go skeleton, fixed architecture, build records and dependency pins; macOS arm64 preset and `.app` bundle | **PASS on Windows 11 x64:** existing evidence retained. **PASS on macOS 26.5.2 / Apple M3 arm64:** shared core compiled; Qt client and `.app` compiled with Xcode 26.6 / Apple Clang 21 / Qt 6.12.0 | Intel macOS and cross-compilation remain unverified |
| P1 native delayed drag | Windows OLE implementation retained. macOS Objective-C++ `NSFilePromiseProvider` bridge implements 3 s file and 15 s ZIP-to-real-folder providers, background operation queue, exact-once completion, session/provider/delegate/lease retention, cancellation cleanup and Qt/AppKit native-event capture. Promise fulfillment writes AppKit's exact destination URL using atomic sibling staging; a nested synchronous file-coordinator call that waited against Finder's receiver coordination was removed. Accepted-but-unrequested promises expire after 60 seconds. The isolated `--p1-file-promises` diagnostic mode does not access production Keychain credentials | **PASS real Finder on macOS 26.5.2 / Apple M3:** 3 s regular file delivered with the expected 38-byte content; 15 s real folder delivered with nested `嵌套/Unicode 名称/内容.txt` and empty `空目录`; after completion Quit exited without the active-Promise warning. Client builds and all 3 core/ZIP tests pass | Cancellation during active download, forced network disconnect, write-permission failure, and post-drop process-lifetime injection remain unverified |
| P2 VPS minimum loop | Pairing, bearer authentication, quota reservation, idempotent intent, streamed PUT/SHA-256, ZIP validation, atomic ready publish, GET, tombstone delete, snapshot, SSE, recovery, expiry GC, verified folder-manifest API, Caddy/systemd examples | **PASS locally and on VPS:** upgraded Linux service is active; trusted HTTPS `/healthz`, authenticated upload/download, snapshot and folder-manifest requests passed | Event pruning/minimum-valid-cursor behavior, concurrent quota race and crash injection remain unverified |
| P3 upload and sync | Windows Credential Manager retained; macOS Security.framework Keychain bridge added. Shared local SQLite, HTTPS, upload and sync code now compiles on macOS and sends `platform=macos` | Existing Windows public-HTTPS evidence retained; macOS client restores existing metadata and snapshot rows without storing the token in SQLite | macOS one-time-token pairing and live two-device SSE/revocation need a fresh pairing token and real verification; persistent retry queue remains pending |
| P4 hybrid cache and content acquisition | Exact `CachePlanner`; sequential real pre-cache downloads; `.part` + length/SHA-256 + atomic publish; corruption revalidation; distinct transfer objects; original-file snapshot/original-folder re-pack validation with VPS fallback; streaming `SafeExtractor` | **PASS:** planner exact cases plus 2,000 randomized sets; public integration deleted both original paths, then downloaded and restored the file and folder with matching bytes, Unicode path and empty directory; malicious traversal and case-collision ZIP tests passed | Durable lease/reference accounting, 4 GiB reservation ledger, cancellation, idle/startup cleanup and dynamic-plan stress remain incomplete |
| P5 integrated native delivery | Real snapshot rows connect to the Windows OLE virtual-file provider. Selection prepares and freezes verified files/folders before drag. The icon view supports rubber-band/Ctrl/Shift multi-selection, preserves the group when drag starts, resolves duplicate export names, and emits one mixed OLE descriptor set for selected files and folder trees | Automated OLE descriptor/memory-stream/file-backed-stream tests pass. The user confirmed the preflight fix resolved own-file, own-folder and cloud-folder delivery failures | Mixed multi-select implementation builds and launches but needs Explorer confirmation; cancellation and post-drag lease lifetime remain incomplete |
| P6 packaging | Release Windows artifacts retained; macOS CMake now builds `DropSpace.app` and generates `DropSpace.icns` from the repository's current `client/resources/icon.jpg`. On macOS, tray activation is split explicitly: left click opens only a compact two-row dashed file surface, while right click owns Open Full Window and Quit | **PASS macOS Debug artifact and menu-bar interaction:** arm64 `.app`, Info.plist icon entry and ICNS resource were generated; the 520×240 two-row left-click surface and separate right-click menu were observed on the Apple M3 host | macOS Release deployment, Qt bundling, signing, notarization, DMG, Intel/universal build remain pending |

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
| macOS implementation status | Active on the `macos` branch; only rows with explicit Apple M3 evidence are marked passed |
| macOS host | 26.5.2 build 25F84, Apple M3, arm64 |
| macOS Xcode / compiler | Xcode 26.6 build 17F113; Apple Clang 21.0.0 |
| macOS CMake / Ninja / Qt | CMake 4.1.1; Ninja 1.11.1; Homebrew Qt 6.12.0 (`qtbase`) |

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
- macOS CTest: 3/3 passed (`cache_planner_tests`, `folder_packer_tests`,
  `safe_extractor_tests`) on macOS 26.5.2 / Apple M3 arm64. The initial macOS
  run exposed and fixed a cross-platform `FolderPacker` bug that compared a
  directory's non-zero macOS metadata size against zero.
- Public Qt integration: `api_client_integration` passed regular-file and folder upload, server validation, snapshot, verified folder manifest, deliberate original-path removal, VPS fallback download and automatic safe folder restoration over `https://43.108.80.211:8443`.
- UI placement: the drop target is now one dashed container containing both its instruction and the live file list; temporary upload rows and synchronized ready rows therefore appear inside the area where the user dropped them. Debug and Release builds passed after this change.
- Explorer-style presentation: the in-zone list uses a 48 px Windows system icon for folders and each file extension, an adaptive icon grid, names below icons, and metadata/status tooltips. Upload-queue placeholders use the same system icons.
- Production UI cleanup: the main surface now contains only the dashed icon manager and a compact bottom action row with Delete and Device/Pairing. Right-clicking an item exposes Save As and confirmed cloud deletion; direct icon drag and double-click Save As remain available. P1 spike controls are hidden from the production layout.
- Blank-space context menu: Refresh requests a new server snapshot; Sort supports name, object bytes, item type/extension and upload time in ascending or descending order; View switches between the 48 px icon grid and a compact detail-style list showing name, type, bytes and local upload time.
- Window controls: the compact action row includes a checkable Always on Top button. It toggles `WindowStaysOnTopHint`, preserves the window position, highlights the active state and changes its label to Cancel Always on Top.
- Multi-selection: icon/list views use extended selection with blank-space rubber-band, Ctrl and Shift selection. All selected objects are prepared independently, dragged as one copy-only OLE operation, and deleted only after one aggregate confirmation; duplicate top-level names receive safe numbered suffixes.
- Selection visuals: list items explicitly remove `ItemIsUserCheckable` and clear `CheckStateRole`; the item-indicator style is zero-sized. Rubber-band and blue selection highlighting remain, while checkbox controls are never shown during selection or drag.
- Production packaging: the executable is `DropSpace.exe` with version 1.0.5 and a Windows resource icon generated from the current white-dog `icon.jpg`; the byte-identical release-directory JPEG is loaded directly for the Qt window/taskbar/tray icon, with an embedded copy as fallback. The minimal portable directory keeps only direct Qt/runtime dependencies plus qwindows, qsqlite, qjpeg, modern Windows style, Schannel and certificate TLS plugins.
- Tray lifecycle: QApplication remains alive after the last window closes; MainWindow converts WM_CLOSE into hide. A QSystemTrayIcon created directly from the unmodified embedded JPEG shows the same MainWindow as a 520×360 frameless tool popup positioned above the tray geometry, with only the dashed file surface visible. The tool-window implementation keeps item and blank-space context menus usable; a deferred deactivate check hides it only when focus genuinely moves outside and preserves it for menus/modal dialogs. The tray context menu separately offers Open Full Window and Full Exit.
- Go: `go test ./...` passed (`internal/api`, `internal/zipcheck`; other packages contain no tests).
- Deployed Linux amd64 server: 11,051,168 bytes, SHA-256 `9cce7633ef079b6907b0b7c80fc495fa96e3dcd474766d9ead805ec2f72455ac`; systemd returned `active` after the manifest-capable upgrade and public `/healthz` returned `{"status":"ok"}`.
- Final release: `E:\workspace\DropSpace_release`, 34,987,398 bytes across 16 files. `DropSpace.exe` is 1,306,611 bytes with SHA-256 `918f7ca036c938037dd6655328701ed92493298eb75f63ccccba6992226e3bc8`; PE metadata reads ProductName `DropSpace`, FileDescription `DropSpace Windows Client`, FileVersion `1.0.5`. The release `icon.jpg` remains byte-identical to the repository image. Debug and Release core suites passed 4/4 after the hover/preselection change, and the packaged client remained running for the three-second dependency smoke test.

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

