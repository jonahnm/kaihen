// SPDX-License-Identifier: GPL-2.0-only
// Host tests of FsHookManager (source/fs_hook.cpp) with a byte buffer in
// place of FS code memory: module-base offset resolution, bridge stubs,
// preimage checks, BTI targets, removal, compaction and failure paths.

#include <cstdio>
#include <cstring>
#include "../source/fs_hook.hpp"

using namespace ounce;

namespace {

constexpr u32 MemorySize = 0x10000;
constexpr u64 ModuleBase = 0x100;
constexpr u64 Target = 0x1000;
constexpr u64 Arena = 0x8000;

constexpr Result BadInput = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result OutOfMemory = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
constexpr Result NotFound = MAKERESULT(Module_Libnx, LibnxError_NotFound);
constexpr Result AlreadyInstalled =
    MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        g_failures++;
    }
}

u32 word(const u8* bytes, u64 address) {
    u32 value = 0;
    std::memcpy(&value, bytes + address, sizeof(value));
    return value;
}

u64 dword(const u8* bytes, u64 address) {
    u64 value = 0;
    std::memcpy(&value, bytes + address, sizeof(value));
    return value;
}

void put(u8* bytes, u64 address, u32 value) {
    std::memcpy(bytes + address, &value, sizeof(value));
}

struct FakeMemory {
    u8 bytes[MemorySize] = {};
    u32 used = 0;
    u32 releases = 0;
    bool fail_allocate = false;
    bool fail_write = false;
};

bool inMemory(u64 address, u32 size) {
    return address <= MemorySize && size <= MemorySize - address;
}

Result fakeRead(void* context, u64 address, void* out, u32 size) {
    auto* memory = static_cast<FakeMemory*>(context);
    if (!inMemory(address, size))
        return BadInput;
    std::memcpy(out, memory->bytes + address, size);
    return 0;
}

Result fakeWrite(void* context, u64 address, const void* bytes, u32 size) {
    auto* memory = static_cast<FakeMemory*>(context);
    if (memory->fail_write || !inMemory(address, size))
        return OutOfMemory;
    std::memcpy(memory->bytes + address, bytes, size);
    return 0;
}

Result fakeReplace(void* context, u64 address, const void* expected,
                   const void* replacement, u32 size) {
    auto* memory = static_cast<FakeMemory*>(context);
    if (!inMemory(address, size) ||
        std::memcmp(memory->bytes + address, expected, size))
        return BadInput;
    return fakeWrite(context, address, replacement, size);
}

Result fakeAllocate(void* context, u32 size, u64* out) {
    auto* memory = static_cast<FakeMemory*>(context);
    if (memory->fail_allocate || memory->used + size > MemorySize - Arena)
        return OutOfMemory;
    *out = Arena + memory->used;
    memory->used += size;
    return 0;
}

void fakeRelease(void* context, u64 address, u32 size) {
    (void)address;
    (void)size;
    static_cast<FakeMemory*>(context)->releases++;
}

FsHookBackend backend(FakeMemory* memory) {
    return {memory, fakeRead, fakeWrite, fakeReplace, fakeAllocate, fakeRelease};
}

void fillNops(u8* bytes, u64 address, u32 count) {
    for (u32 index = 0; index < count; index++)
        put(bytes, address + index * 4, 0xD503201Fu);
}

void testInstallAndRemove() {
    FakeMemory memory;
    fillNops(memory.bytes, Target, 8);
    put(memory.bytes, Target, 0xD503233Fu); // PACIASP

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};
    Result rc = manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info);
    expect(R_SUCCEEDED(rc), "hook installs");
    expect(info.id == 1 && info.state == HEN_FS_HOOK_INSTALLED, "hook identity");
    expect(info.target == Target, "hook target");
    expect(info.replacement == Arena, "hook replacement is the bridge stub");
    expect(info.trampoline == Arena + hook::StubTrampolineOffset,
           "hook gets a trampoline after the bridge data");
    expect(manager.count() == 1, "hook count");
    expect((word(memory.bytes, Target) & 0xFC000000u) == 0x14000000u,
           "target starts with a branch");
    expect(word(memory.bytes, info.trampoline) == 0xD503249Fu &&
           word(memory.bytes, info.trampoline + 4) == 0xD503233Fu,
           "trampoline has the landing pad and the PAC prologue");
    expect(dword(memory.bytes, Arena + hook::StubDataOffset) == info.trampoline,
           "bridge parameter block points at its trampoline");
    expect(word(memory.bytes, Arena + hook::StubDataOffset + 16) == 1,
           "bridge parameter block carries the hook id");
    expect(!std::memcmp(memory.bytes + Arena + hook::StubDataOffset + hook::StubPortOffset,
                        "kaih:fs", hook::StubPortSize),
           "bridge parameter block carries the port name");
    expect(word(memory.bytes, Arena) == 0xD503249Fu,
           "bridge starts with a BTI landing pad");
    expect(word(memory.bytes, Arena + 4) == 0xD10803FFu,
           "bridge reserves its frame after the landing pad");

    HenFsHookInfo listed = {};
    expect(R_SUCCEEDED(manager.info(0, &listed)) && listed.id == info.id,
           "hook is listed by index");
    expect(manager.info(1, &listed) == NotFound, "missing index");

    u32 owner = 0;
    u32 handler_id = 0;
    expect(R_SUCCEEDED(manager.callInfo(info.id, &owner, &handler_id)) &&
           owner == 9 && handler_id == 7,
           "hook knows its plugin owner and handler");
    expect(manager.callInfo(99, &owner, &handler_id) == NotFound,
           "missing callback info");

    expect(manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info) == AlreadyInstalled,
           "duplicate target is refused");

    expect(R_SUCCEEDED(manager.remove(info.id)), "hook removes");
    expect(manager.count() == 0 && memory.releases == 1, "removal frees the slot");
    expect(word(memory.bytes, Target) == 0xD503233Fu, "removal restores the target");
    expect(manager.remove(info.id) == NotFound, "removing twice fails");
}

void testOffsetAndBtiTargets() {
    FakeMemory memory;
    fillNops(memory.bytes, Target, 8);
    fillNops(memory.bytes, 0x2000, 8);
    put(memory.bytes, 0x2000, 0xD503245Fu); // BTI c
    put(memory.bytes, 0x2004, 0xD503233Fu); // PACIASP

    FsHookManager manager;
    manager.setBackend(backend(&memory));

    HenFsHookInfo info = {};
    expect(R_SUCCEEDED(manager.install({Target - ModuleBase, 3, 0, 0}, ModuleBase, 9, &info)),
           "module-base offset installs");
    expect(info.target == Target, "target offset resolves from the module base");

    HenFsHookInfo bti = {};
    expect(R_SUCCEEDED(manager.install({0x1f00, 4, 0, 0}, ModuleBase, 9, &bti)),
           "BTI target installs");
    expect(bti.target == 0x2000, "BTI target address");
    expect(word(memory.bytes, 0x2000) == 0xD503245Fu,
           "BTI landing pad is kept");
    expect((word(memory.bytes, 0x2004) & 0xFC000000u) == 0x14000000u,
           "patch starts after the landing pad");
    expect(word(memory.bytes, bti.trampoline + 4) == 0xD503233Fu,
           "trampoline starts after the landing pad with the PAC prologue");
}

void testFailures() {
    FakeMemory memory;
    fillNops(memory.bytes, Target, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};

    expect(manager.install({Target - ModuleBase + 1, 7, 0, 0}, ModuleBase, 9, &info) == BadInput,
           "unaligned target is refused");
    expect(manager.install({Target - ModuleBase, 0, 0, 0}, ModuleBase, 9, &info) == BadInput,
           "missing handler id is refused");
    expect(manager.install({Target - ModuleBase, 7, 1, 0}, ModuleBase, 9, &info) == BadInput,
           "flags are refused");
    expect(manager.install({Target - ModuleBase, 7, 0, 0}, 0, 9, &info) == BadInput,
           "missing module base is refused");
    expect(manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 0, &info) == BadInput,
           "missing plugin owner is refused");
    expect(manager.install({0xfef8, 7, 0, 0}, ModuleBase, 9, &info) == BadInput,
           "unreadable target is refused");

    memory.fail_allocate = true;
    expect(manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info) == OutOfMemory,
           "allocation failure is reported");
    memory.fail_allocate = false;

    memory.fail_write = true;
    expect(manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info) == OutOfMemory,
           "bridge write failure is reported");
    memory.fail_write = false;
    expect(manager.count() == 0, "failed installs leave no hook");
    expect(word(memory.bytes, Target) == 0xD503201Fu,
           "failed installs leave the target alone");

    expect(R_SUCCEEDED(manager.install({Target - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info)),
           "hook installs before the preimage test");
    put(memory.bytes, Target, 0xD503201Fu);
    expect(manager.remove(info.id) == BadInput,
           "removal refuses a changed preimage");
    expect(manager.count() == 1, "failed removal keeps the hook listed");
    put(memory.bytes, Target, 0xD5032440u); // Keep the test independent of the patch encoding.
}

void testFullTableAndCompaction() {
    FakeMemory memory;
    for (u32 index = 0; index < FsHookManager::MaxHooks; index++)
        fillNops(memory.bytes, Target + index * 0x20, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};
    for (u32 index = 0; index < FsHookManager::MaxHooks; index++) {
        expect(R_SUCCEEDED(manager.install(
                   {Target + index * 0x20 - ModuleBase, index + 1, 0, 0},
                   ModuleBase, 9, &info)),
               "table fills");
        expect(info.id == index + 1, "ids increase");
    }
    expect(manager.count() == FsHookManager::MaxHooks, "full count");
    expect(manager.install({0x3000 - ModuleBase, 7, 0, 0}, ModuleBase, 9, &info) ==
               OutOfMemory,
           "a full table is refused");

    // Remove the first hook: the rest shift down but keep their IDs.
    expect(R_SUCCEEDED(manager.remove(1)), "first hook removes");
    expect(manager.count() == FsHookManager::MaxHooks - 1, "count after removal");
    expect(R_SUCCEEDED(manager.info(0, &info)) && info.id == 2,
           "next hook becomes index 0");
    expect(R_SUCCEEDED(manager.info(FsHookManager::MaxHooks - 2, &info)) &&
           info.id == FsHookManager::MaxHooks,
           "last hook stays listed");

    for (u32 id = 2; id <= FsHookManager::MaxHooks; id++)
        expect(R_SUCCEEDED(manager.remove(id)), "remaining hooks remove");
    expect(manager.count() == 0, "empty after removing all");
}

} // namespace

int main() {
    testInstallAndRemove();
    testOffsetAndBtiTargets();
    testFailures();
    testFullTableAndCompaction();
    if (g_failures) {
        std::printf("FS hook test: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("FS hook test: PASS\n");
    return 0;
}
