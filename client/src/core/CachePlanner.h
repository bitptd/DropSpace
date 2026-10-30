#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace dropspace {

inline constexpr std::int64_t kPrecacheLimitBytes = 52'428'800;

struct CacheCandidate {
  std::string id;
  std::string uploadDeviceId;
  std::int64_t objectSizeBytes = -1;
  std::chrono::system_clock::time_point createdAt{};
  std::chrono::system_clock::time_point expiresAt{};
  bool ready = false;
  bool deleted = false;
};

struct CachePlan {
  std::vector<std::string> selectedIds;
  std::int64_t reservedBytes = 0;
};

[[nodiscard]] CachePlan planPrecache(
    const std::vector<CacheCandidate>& files,
    const std::string& currentDeviceId,
    std::chrono::system_clock::time_point now,
    std::int64_t limitBytes = kPrecacheLimitBytes);

}  // namespace dropspace

