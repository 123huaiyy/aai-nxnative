#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "x86/lifted.h"

namespace recomp {

// Most x64 function entry points are 16-byte aligned. Keep one directly
// indexed pointer per aligned address and store exact unaligned entries apart.
class LiftedIndex {
 public:
  LiftedIndex() = default;

  LiftedIndex(uint64_t base, uint64_t size, std::span<const LiftedFn> entries)
      : aligned_((size + kAlignment - 1) / kAlignment, nullptr) {
    for (const auto& entry : entries) {
      const uint64_t offset = entry.va - base;
      if (offset >= size) continue;
      if (offset % kAlignment == 0)
        aligned_[offset / kAlignment] = entry.fn;
      else
        unaligned_.push_back({offset, entry.fn});
    }
    std::stable_sort(unaligned_.begin(), unaligned_.end(),
                     [](const LiftedFn& a, const LiftedFn& b) { return a.va < b.va; });
  }

  // The caller checks that offset is inside this code range.
  GuestFn Find(uint64_t offset) const {
    if (offset % kAlignment == 0) return aligned_[offset / kAlignment];
    auto next = std::upper_bound(unaligned_.begin(), unaligned_.end(), offset,
                                 [](uint64_t va, const LiftedFn& e) { return va < e.va; });
    if (next == unaligned_.begin()) return nullptr;
    const auto& entry = *--next;
    return entry.va == offset ? entry.fn : nullptr;
  }

  size_t AllocatedBytes() const {
    return aligned_.capacity() * sizeof(GuestFn) + unaligned_.capacity() * sizeof(LiftedFn);
  }

 private:
  static constexpr uint64_t kAlignment = 16;
  std::vector<GuestFn> aligned_;
  std::vector<LiftedFn> unaligned_;
};

}  // namespace recomp
