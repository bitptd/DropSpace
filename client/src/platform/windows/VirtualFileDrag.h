#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dropspace::windows {

struct VirtualFileEntry {
  std::wstring relativePath;
  bool directory = false;
  std::uint64_t size = 0;
  std::function<std::vector<std::byte>()> provideBytes;
  // Preferred for real objects: prepares and returns an immutable local file.
  // The OLE stream reads it by offset without loading the whole object in RAM.
  std::function<std::wstring()> provideFilePath;
};

// Starts a copy-only OLE drag. File data is requested lazily through
// CFSTR_FILECONTENTS/IStream; no placeholder path or target directory is used.
[[nodiscard]] bool startVirtualFileDrag(std::vector<VirtualFileEntry> entries,
                                        std::wstring* errorMessage = nullptr);

// Exercises descriptor enumeration, lazy stream reads, seek/clone, and async
// lifetime without pretending to be an Explorer interoperability test.
[[nodiscard]] bool selfTestVirtualFileProvider(std::wstring* errorMessage = nullptr);

}  // namespace dropspace::windows

#endif

