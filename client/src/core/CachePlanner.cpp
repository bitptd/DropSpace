#include "core/CachePlanner.h"

#include <algorithm>
#include <limits>

namespace dropspace {

CachePlan planPrecache(const std::vector<CacheCandidate>& files,
                       const std::string& currentDeviceId,
                       std::chrono::system_clock::time_point now,
                       std::int64_t limitBytes) {
  CachePlan plan;
  if (currentDeviceId.empty() || limitBytes < 0) {
    return plan;
  }

  std::vector<const CacheCandidate*> candidates;
  candidates.reserve(files.size());
  for (const auto& file : files) {
    if (file.ready && !file.deleted && file.expiresAt > now &&
        file.uploadDeviceId != currentDeviceId &&
        file.objectSizeBytes >= 0) {
      candidates.push_back(&file);
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const CacheCandidate* left, const CacheCandidate* right) {
              if (left->objectSizeBytes != right->objectSizeBytes) {
                return left->objectSizeBytes < right->objectSizeBytes;
              }
              if (left->createdAt != right->createdAt) {
                return left->createdAt < right->createdAt;
              }
              return left->id < right->id;
            });

  for (const auto* file : candidates) {
    // Subtraction avoids signed overflow from used + candidate.
    if (file->objectSizeBytes <= limitBytes - plan.reservedBytes) {
      plan.selectedIds.push_back(file->id);
      plan.reservedBytes += file->objectSizeBytes;
    }
  }
  return plan;
}

}  // namespace dropspace

