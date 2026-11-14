// SPDX-License-Identifier: GPL-2.0-only
// Host tests of FsArena (source/fs_arena.hpp): first-fit allocation,
// alignment, release and reuse, entry limits and invalid ranges.

#include <cstdint>
#include <cstdio>
#include "../source/fs_arena.hpp"

using namespace ounce;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        g_failures++;
    }
}

void testFirstFit() {
    FsArena arena;
    expect(!arena.ready(), "a fresh arena is not ready");
    arena.reset(0x1000, 0x1000);
    expect(arena.ready(), "reset makes the arena usable");

    u64 first = 0;
    u64 second = 0;
    u64 third = 0;
    u64 other = 0;
    expect(arena.allocate(0x400, &first) && first == 0x1000,
           "the first allocation starts at the base");
    expect(arena.allocate(0x300, &second) && second == 0x1400,
           "the second allocation follows the first");
    expect(arena.allocate(0x500, &third) && third == 0x1700,
           "the third allocation follows the second");
    expect(!arena.allocate(0x1000, &other),
           "a full arena rejects a large allocation");

    arena.release(second, 0x300);
    expect(arena.allocate(0x300, &second) && second == 0x1400,
           "a freed gap is reused");
    expect(arena.allocate(1, &other) && other == 0x1c00,
           "a one-byte allocation takes the next gap");
    expect((other & (FsArena::Alignment - 1)) == 0,
           "allocations are aligned");
}

void testExactFit() {
    FsArena arena;
    arena.reset(0x3000, 0x20);
    u64 first = 0;
    u64 other = 0;
    expect(arena.allocate(0x20, &first) && first == 0x3000,
           "an exact-fit allocation succeeds");
    expect(!arena.allocate(0x10, &other), "an exact fit leaves no room");
}

void testSizes() {
    FsArena arena;
    arena.reset(0x2000, 0x20);
    u64 address = 0;
    u64 other = 0;
    expect(FsArena::roundUp(1) == 0x10, "sizes round up to the alignment");
    expect(FsArena::roundUp(0x10) == 0x10, "aligned sizes stay");
    expect(FsArena::roundUp(0x11) == 0x20, "sizes round to the next multiple");
    expect(!arena.allocate(0, &address), "zero size fails");
    expect(arena.allocate(0x20, &address), "the arena is allocated");
    arena.release(address, 0x10);
    expect(!arena.allocate(0x20, &other),
           "a wrong-size release keeps the range");
    arena.release(address, 0x20);
    expect(arena.allocate(0x20, &other) && other == address,
           "a correct release frees the range");
}

void testAlignment() {
    FsArena arena;
    arena.reset(0x5000, 0x20);
    u64 first = 0;
    u64 second = 0;
    u64 third = 0;
    expect(arena.allocate(1, &first) && first == 0x5000,
           "a one-byte allocation starts at the base");
    expect(arena.allocate(1, &second) && second == 0x5010,
           "rounding up moves to the next alignment");
    expect(!arena.allocate(1, &third), "rounded-up sizes still fill the arena");
}

void testLimits() {
    FsArena arena;
    arena.reset(0x10000, 0x1000);
    u64 addresses[FsArena::MaxRanges] = {};
    for (u32 index = 0; index < FsArena::MaxRanges; index++) {
        expect(arena.allocate(0x10, &addresses[index]),
               "the arena records MaxRanges allocations");
    }
    u64 other = 0;
    expect(!arena.allocate(0x10, &other), "the arena runs out of entries");
    arena.release(addresses[0], 0x10);
    expect(arena.allocate(0x10, &other) && other == 0x10000,
           "a released entry is available again");
}

void testInvalidRanges() {
    FsArena arena;
    u64 address = 0;
    arena.reset(0, 0x1000);
    expect(!arena.ready(), "a zero base is not ready");
    arena.reset(UINT64_MAX - 0x10, 0x100);
    expect(!arena.ready(), "a wrapping range is not ready");
    expect(!arena.allocate(0x10, &address),
           "an unusable arena allocates nothing");
    arena.reset(0x4000, 0);
    expect(!arena.ready(), "a zero size is not ready");
}

} // namespace

int main() {
    testFirstFit();
    testExactFit();
    testSizes();
    testAlignment();
    testLimits();
    testInvalidRanges();
    if (g_failures) {
        std::printf("FS arena test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("FS arena test: PASS\n");
    return 0;
}
