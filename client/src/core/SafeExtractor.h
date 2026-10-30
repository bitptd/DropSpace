#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace dropspace {

struct ExtractLimits {
  std::int64_t maxExpandedBytes = 4LL * 1024 * 1024 * 1024;
  std::int64_t maxEntries = 100000;
  int maxDepth = 64;
  double maxCompressionRatio = 1000.0;
};

struct ExtractedEntry {
  QString relativePath;
  bool directory = false;
  std::int64_t size = 0;
};

[[nodiscard]] bool safeExtractZip(
    const QString& zipPath, const QString& destination,
    std::vector<ExtractedEntry>* manifest, QString* error,
    ExtractLimits limits = {},
    const std::function<bool()>& cancelled = {});

}  // namespace dropspace
