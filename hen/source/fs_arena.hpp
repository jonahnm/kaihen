// SPDX-License-Identifier: GPL-2.0-only
// The writable FS arena that RO finds for HEN. It is not FS's heap: hook
// bridge slots and the plugin allocation command share this one range, so
// HEN tracks every allocation itself.
#pragma once

#include <cstdint>
#include <switch/types.h>

namespace ounce {

/// A first-fit allocator over one fixed range, 16-byte aligned.
class FsArena {
  public:
    /// The live ranges that the allocator can track at once. Sixteen hook
    /// slots and sixteen plugin allocations fit.
    static constexpr u32 MaxRanges = 32;
    /// The alignment of every returned address.
    static constexpr u64 Alignment = 16;

    /// Rounds a size up to Alignment.
    static constexpr u32 roundUp(u32 size) {
        return static_cast<u32>((static_cast<u64>(size) + Alignment - 1) &
                                ~(Alignment - 1));
    }

    FsArena() = default;
    FsArena(const FsArena&) = delete;
    FsArena& operator=(const FsArena&) = delete;

    /// Sets the range and forgets every allocation. An empty or wrapping
    /// range leaves the arena unusable.
    void reset(u64 base, u64 size) {
        for (Range& range : ranges_)
            range = {};
        if (!base || !size || base > UINT64_MAX - size) {
            base_ = 0;
            size_ = 0;
            return;
        }
        base_ = base;
        size_ = size;
    }

    bool ready() const { return base_ != 0 && size_ != 0; }

    /// Allocates @p size bytes at an Alignment-aligned address. False when
    /// no gap is large enough or all MaxRanges entries are in use.
    bool allocate(u32 size, u64* out) {
        if (!out || !size || !ready())
            return false;

        const u64 aligned = roundUp(size);
        if (aligned > size_)
            return false;

        Range* entry = nullptr;
        for (Range& range : ranges_) {
            if (!range.size) {
                entry = &range;
                break;
            }
        }
        if (!entry)
            return false;

        const u64 end = base_ + size_;
        u64 candidate = base_;
        // Move the candidate past every recorded range that the proposed
        // allocation would overlap, until none does: the first gap that
        // fits. The range checks use subtraction so that a candidate near
        // the top of the address space cannot overflow.
        for (u32 pass = 0; pass <= MaxRanges; pass++) {
            bool moved = false;
            for (const Range& range : ranges_) {
                if (!range.size)
                    continue;
                const bool ends_after = candidate < range.address + range.size;
                const bool starts_before_end =
                    range.address <= candidate ||
                    range.address - candidate < aligned;
                if (ends_after && starts_before_end) {
                    candidate = range.address + range.size;
                    moved = true;
                }
            }
            if (!moved)
                break;
        }
        if (candidate > end || aligned > end - candidate)
            return false;

        *entry = {candidate, static_cast<u32>(aligned), 0};
        *out = candidate;
        return true;
    }

    /// Frees the allocation that starts at @p address and is @p size bytes.
    /// A range that is not recorded, or a different size, is ignored.
    void release(u64 address, u32 size) {
        if (!address || !size)
            return;
        for (Range& range : ranges_) {
            if (range.size == size && range.address == address) {
                range = {};
                return;
            }
        }
    }

  private:
    struct Range {
        u64 address = 0;
        u32 size = 0;
        u32 reserved = 0;
    };

    Range ranges_[MaxRanges] = {};
    u64 base_ = 0;
    u64 size_ = 0;
};

} // namespace ounce
