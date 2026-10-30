#include "core/CachePlanner.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using dropspace::CacheCandidate;
using Clock = std::chrono::system_clock;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void expect(bool value, const std::string& message) {
  if (!value) fail(message);
}

CacheCandidate item(std::string id, std::int64_t bytes,
                    std::string device = "remote",
                    Clock::time_point created = Clock::time_point{}) {
  const auto now = Clock::time_point{} + std::chrono::hours(24);
  return {.id = std::move(id),
          .uploadDeviceId = std::move(device),
          .objectSizeBytes = bytes,
          .createdAt = created,
          .expiresAt = now + std::chrono::hours(1),
          .ready = true,
          .deleted = false};
}

void exactCases() {
  const auto now = Clock::time_point{} + std::chrono::hours(24);
  expect(dropspace::planPrecache({}, "me", now).selectedIds.empty(),
         "empty input");

  auto own = item("own", 1, "me");
  auto remote = item("remote", 49LL * 1024 * 1024);
  auto plan = dropspace::planPrecache({own, remote}, "me", now);
  expect(plan.selectedIds == std::vector<std::string>{"remote"},
         "current device must be excluded");

  const auto mib = 1024LL * 1024;
  plan = dropspace::planPrecache(
      {item("60", 60 * mib), item("30", 30 * mib), item("15", 15 * mib),
       item("10", 10 * mib)},
      "me", now);
  expect(plan.selectedIds == std::vector<std::string>({"10", "15"}),
         "10/15/30/60 greedy selection");
  expect(plan.reservedBytes == 25 * mib, "greedy byte accounting");

  plan = dropspace::planPrecache(
      {item("exact", dropspace::kPrecacheLimitBytes)}, "me", now);
  expect(plan.selectedIds.size() == 1, "exact limit must fit");

  plan = dropspace::planPrecache(
      {item("over", dropspace::kPrecacheLimitBytes + 1)}, "me", now);
  expect(plan.selectedIds.empty(), "one byte over limit must not fit");

  auto deleted = item("deleted", 0);
  deleted.deleted = true;
  auto expired = item("expired", 0);
  expired.expiresAt = now;
  auto uploading = item("uploading", 0);
  uploading.ready = false;
  plan = dropspace::planPrecache({deleted, expired, uploading}, "me", now);
  expect(plan.selectedIds.empty(), "ineligible metadata must be excluded");

  plan = dropspace::planPrecache(
      {item("b", 1, "remote", Clock::time_point{}),
       item("a", 1, "remote", Clock::time_point{})},
      "me", now);
  expect(plan.selectedIds == std::vector<std::string>({"a", "b"}),
         "tie ordering must be stable");
}

void randomizedProperties() {
  const auto now = Clock::time_point{} + std::chrono::hours(24);
  std::mt19937_64 random(0x44524f5053504143ULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::vector<CacheCandidate> input;
    const int count = static_cast<int>(random() % 100);
    for (int index = 0; index < count; ++index) {
      auto candidate = item(std::to_string(index),
                            static_cast<std::int64_t>(random() % (70ULL * 1024 * 1024)));
      if (random() % 11 == 0) candidate.uploadDeviceId = "me";
      if (random() % 13 == 0) candidate.deleted = true;
      if (random() % 17 == 0) candidate.ready = false;
      input.push_back(std::move(candidate));
    }

    const auto first = dropspace::planPrecache(input, "me", now);
    std::shuffle(input.begin(), input.end(), random);
    const auto second = dropspace::planPrecache(input, "me", now);
    expect(first.selectedIds == second.selectedIds,
           "input ordering cannot change a plan");
    expect(first.reservedBytes <= dropspace::kPrecacheLimitBytes,
           "plan exceeded fixed limit");
    for (const auto& id : first.selectedIds) {
      const auto found = std::find_if(input.begin(), input.end(),
                                      [&](const auto& value) { return value.id == id; });
      expect(found != input.end() && found->ready && !found->deleted &&
                 found->uploadDeviceId != "me" && found->expiresAt > now &&
                 found->objectSizeBytes >= 0,
             "selected item was ineligible");
    }
  }
}

}  // namespace

int main() {
  exactCases();
  randomizedProperties();
  std::cout << "cache_planner_tests: 2000 randomized sets passed\n";
  return 0;
}

