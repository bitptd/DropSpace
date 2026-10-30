#pragma once

#include <QString>

#include <cstdint>

namespace dropspace {

struct PackedFolder {
  QString zipPath;
  QString objectSha256;
  QString treeSha256;
  std::int64_t objectSizeBytes = 0;
  std::int64_t expandedSizeBytes = 0;
  std::int64_t entryCount = 0;
};

struct FolderLimits {
  std::int64_t maxExpandedBytes = 4LL * 1024 * 1024 * 1024;
  std::int64_t maxEntries = 10'000;
  int maxDepth = 64;
};

// Scans and freezes a directory, rejects links/reparse points and names that
// cannot be restored safely on Windows/default macOS volumes, then writes one
// standard ZIP object. The ZIP contains the root's children, not the root.
[[nodiscard]] bool packFolder(const QString& sourceRoot,
                              const QString& outputZipPart,
                              PackedFolder* result,
                              QString* error,
                              FolderLimits limits = {});

}  // namespace dropspace

