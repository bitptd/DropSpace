# Windows native drag spike

Status: compiled and provider self-test passed on Windows 11 x64. On 2026-10-29 the user reported that both the 3-second virtual file and 15-second nested Unicode folder-tree prototypes worked correctly in real Windows Explorer.

## Prototype

`client/src/platform/windows/VirtualFileDrag.cpp` implements a copy-only OLE data source with:

- `FILEGROUPDESCRIPTORW` metadata generated without network or disk I/O;
- per-index `CFSTR_FILECONTENTS` using `TYMED_ISTREAM`;
- a lazy worker-backed read-only `IStream` with independent cloned seek positions and 64-bit sizes;
- `IDataObjectAsyncCapability` lifetime negotiation;
- directory descriptors and nested relative member descriptors for the folder-tree Explorer spike;
- no nonexistent paths, URLs, zero-byte placeholders, target-window inspection, or target-directory assumptions.

The Qt spike window exposes a 3-second delayed regular file and a 15-second delayed directory tree containing a nested Unicode name and an empty directory. The provider starts work when Explorer requests a member stream; Explorer's consumer thread may wait, but the payload producer never waits on the Qt UI thread.

## Required manual evidence

`windows_virtual_file_tests` exercised format generation, the delayed worker-backed stream, `Read`, `Seek`, `Clone`, `Stat`, and async mode on Windows. The user-reported Explorer pass adds target evidence, while the following cases remain explicitly **not verified**:

- mixed selection, duplicate names, cancellation, failed providers, post-drag reads, and third-party targets;
- localhost HTTP acquisition and ZIP-to-safe-folder delivery (the current directory-tree case is synthetic and is not yet a ZIP extractor test).

Do not treat successful compilation or format enumeration unit tests as Explorer delivery evidence.

## macOS Finder File Promise spike

Status on 2026-10-30: **implemented and compiled on macOS 26.5.2 / Apple M3
(arm64), but final Finder delivery remains unverified**. The desktop automation
can initiate ordinary Qt drags, but it cannot complete a cross-application
AppKit `NSDraggingSession`; therefore no Finder pass is claimed.

`client/src/platform/macos/FilePromiseDrag.mm` implements:

- one `NSFilePromiseProvider` and `NSDraggingItem` per frozen top-level entry;
- copy-only `NSDraggingSource` behavior and stable, collision-free export names;
- a dedicated background `NSOperationQueue`, so the Qt/AppKit main thread does
  not perform the 3-second or 15-second preparation;
- exact-once completion, unpublished sibling `.part` output for ordinary files,
  and `SafeExtractor` atomic directory publication for folder ZIP objects;
- a session owner that retains providers, delegates, callbacks and content
  leases until all Promise callbacks finish, and releases immediately when a
  drag is cancelled before any Promise begins;
- an AppKit local mouse-event monitor installed before the first press. This is
  required because after Qt wraps the native event, `NSApp.currentEvent` is not
  guaranteed to remain a left-mouse drag event. A user-reported
  `File Promise drag requires the current mouse drag event` failure was fixed by
  retaining the current window's latest valid left-down/left-drag event.

Run the P1 surface with:

```bash
build/macos-arm64-debug/client/DropSpace.app/Contents/MacOS/DropSpace \
  --p1-file-promises
```

It adds a 3-second regular-file Promise and a 15-second folder Promise. The
folder is generated as a ZIP containing `嵌套/Unicode 名称/内容.txt` and
`空目录`, then delivered through the same `SafeExtractor` used by real objects.

Still required in a human-operated Finder session: drop both entries into a
fresh directory; verify the regular-file SHA-256; verify the delivered folder is
a real directory with the nested Unicode file and empty directory; then repeat
cancellation, failure/disconnect, mixed selection and post-drag lifetime cases.

