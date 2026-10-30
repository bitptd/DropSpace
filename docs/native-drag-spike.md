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

