// SPDX-License-Identifier: GPL-2.0-only
// Host checks of the FS bridge stub's emitted bytes and parameter block.

#include <cstdio>
#include <cstring>
#include <switch/types.h>
#include "../source/fs_hook_stub.hpp"

using namespace ounce;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        g_failures++;
    }
}

u32 word(const u8* bytes, u32 offset) {
    u32 value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

u64 dword(const u8* bytes, u32 offset) {
    u64 value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void testLayout() {
    constexpr u32 HookId = 42;
    constexpr u64 Trampoline = 0x12345678;
    u8 stub[hook::StubSize + 16] = {};
    std::memset(stub, 0xaa, sizeof(stub));

    expect(hook::BuildStub(HookId, Trampoline, stub, sizeof(stub)),
           "stub builds");
    expect(word(stub, 0) == 0xD503249Fu,
           "stub starts with a BTI landing pad");
    expect(word(stub, 4) == 0xD10803FFu,
           "stub reserves a 0x200-byte frame");
    expect(dword(stub, hook::StubDataOffset) == Trampoline,
           "stub stores its trampoline");
    expect(dword(stub, hook::StubDataOffset + 8) == 0,
           "stub starts without a session");
    expect(word(stub, hook::StubDataOffset + 16) == HookId,
           "stub stores its hook id");
    expect(!std::memcmp(stub + hook::StubDataOffset + hook::StubPortOffset,
                        "kaih:fs", hook::StubPortSize),
           "stub stores the port name");

    u8 too_small[hook::StubSize] = {};
    expect(!hook::BuildStub(HookId, Trampoline, too_small, sizeof(too_small) - 1),
           "a short buffer is refused");
}

} // namespace

int main() {
    testLayout();
    if (g_failures) {
        std::printf("FS hook stub test: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("FS hook stub test: PASS\n");
    return 0;
}
