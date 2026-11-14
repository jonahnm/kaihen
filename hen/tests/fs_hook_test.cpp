// SPDX-License-Identifier: GPL-2.0-only
// Host tests of FsHookManager (source/fs_hook.cpp) with a byte buffer in
// place of FS code memory: module-base offset resolution, preimage checks,
// BTI targets, removal, compaction and the failure paths.

#include <cstdio>
#include <cstring>
#include "../source/fs_hook.hpp"

using namespace ounce;

namespace {

constexpr u32 MemorySize = 0x10000;
constexpr u64 ModuleBase = 0x100;
constexpr u64 Replacement = 0x300;
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
    fillNops(memory.bytes, 0x100, 8);
    put(memory.bytes, 0x100, 0xD503233Fu); // PACIASP
    fillNops(memory.bytes, Replacement, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};
    Result rc = manager.install({0, Replacement - ModuleBase, 0, 0}, ModuleBase, &info);
    expect(R_SUCCEEDED(rc), "hook installs");
    expect(info.id == 1 && info.state == HEN_FS_HOOK_INSTALLED, "hook identity");
    expect(info.target == 0x100 && info.replacement == Replacement, "hook addresses");
    expect(info.trampoline == Arena, "hook gets the first trampoline slot");
    expect(manager.count() == 1, "hook count");
    expect((word(memory.bytes, 0x100) & 0xFC000000u) == 0x14000000u,
           "target starts with a branch");
    expect(word(memory.bytes, Arena) == 0xD503249Fu &&
           word(memory.bytes, Arena + 4) == 0xD503233Fu,
           "trampoline has the landing pad and the PAC prologue");

    HenFsHookInfo listed = {};
    expect(R_SUCCEEDED(manager.info(0, &listed)) && listed.id == info.id,
           "hook is listed by index");
    expect(manager.info(1, &listed) == NotFound, "missing index");

    expect(manager.install({0, Replacement - ModuleBase, 0, 0}, ModuleBase, &info) == AlreadyInstalled,
           "duplicate target is refused");

    expect(R_SUCCEEDED(manager.remove(info.id)), "hook removes");
    expect(manager.count() == 0 && memory.releases == 1, "removal frees the slot");
    expect(word(memory.bytes, 0x100) == 0xD503233Fu, "removal restores the target");
    expect(manager.remove(info.id) == NotFound, "removing twice fails");
}

void testOffsetAndBtiTargets() {
    FakeMemory memory;
    fillNops(memory.bytes, 0x100, 8);
    fillNops(memory.bytes, 0x200, 8);
    put(memory.bytes, 0x200, 0xD503245Fu); // BTI c
    put(memory.bytes, 0x204, 0xD503233Fu); // PACIASP
    fillNops(memory.bytes, Replacement, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));

    HenFsHookInfo info = {};
    expect(R_SUCCEEDED(manager.install({0, Replacement - ModuleBase, 0, 0},
                                       ModuleBase, &info)),
           "module-base offset installs");
    expect(info.target == 0x100, "target offset resolves from the module base");
    expect(info.replacement == Replacement,
           "replacement offset resolves from the module base");

    HenFsHookInfo bti = {};
    expect(R_SUCCEEDED(manager.install({0x100, Replacement - ModuleBase, 0, 0},
                                       ModuleBase, &bti)),
           "BTI target installs");
    expect(word(memory.bytes, 0x200) == 0xD503245Fu,
           "BTI landing pad is kept");
    expect((word(memory.bytes, 0x204) & 0xFC000000u) == 0x14000000u,
           "patch starts after the landing pad");
    expect(word(memory.bytes, bti.trampoline + 4) == 0xD503233Fu,
           "trampoline starts after the landing pad with the PAC prologue");
}

void testFailures() {
    FakeMemory memory;
    fillNops(memory.bytes, 0x100, 8);
    fillNops(memory.bytes, Replacement, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};

    expect(manager.install({0x001, Replacement - ModuleBase, 0, 0},
                           ModuleBase, &info) == BadInput,
           "unaligned target is refused");
    expect(manager.install({0, Replacement - ModuleBase + 2, 0, 0},
                           ModuleBase, &info) == BadInput,
           "unaligned replacement is refused");
    expect(manager.install({0, Replacement - ModuleBase, 1, 0},
                           ModuleBase, &info) == BadInput,
           "flags are refused");
    expect(manager.install({0, 0, 0, 0}, 0, &info) == BadInput,
           "missing module base is refused");
    expect(manager.install({0xFEF8, Replacement - ModuleBase, 0, 0},
                           ModuleBase, &info) == BadInput,
           "unreadable target is refused");

    memory.fail_allocate = true;
    expect(manager.install({0, Replacement - ModuleBase, 0, 0},
                           ModuleBase, &info) == OutOfMemory,
           "allocation failure is reported");
    memory.fail_allocate = false;

    memory.fail_write = true;
    expect(manager.install({0, Replacement - ModuleBase, 0, 0},
                           ModuleBase, &info) == OutOfMemory,
           "trampoline write failure is reported");
    memory.fail_write = false;
    expect(manager.count() == 0, "failed installs leave no hook");
    expect(word(memory.bytes, 0x100) == 0xD503201Fu,
           "failed installs leave the target alone");

    expect(R_SUCCEEDED(manager.install({0, Replacement - ModuleBase, 0, 0},
                                       ModuleBase, &info)),
           "hook installs before the preimage test");
    put(memory.bytes, 0x100, 0xD503201Fu);
    expect(manager.remove(info.id) == BadInput,
           "removal refuses a changed preimage");
    expect(manager.count() == 1, "failed removal keeps the hook listed");
    put(memory.bytes, 0x100, 0xD5032440u); // Not the patch, so the test keeps a clean state.
}

void testFullTableAndCompaction() {
    FakeMemory memory;
    for (u32 index = 0; index < FsHookManager::MaxHooks; index++)
        fillNops(memory.bytes, 0x1000 + index * 0x20, 8);
    fillNops(memory.bytes, Replacement, 8);

    FsHookManager manager;
    manager.setBackend(backend(&memory));
    HenFsHookInfo info = {};
    for (u32 index = 0; index < FsHookManager::MaxHooks; index++) {
        expect(R_SUCCEEDED(manager.install(
                   {0x1000 + index * 0x20 - ModuleBase,
                    Replacement - ModuleBase, 0, 0}, ModuleBase, &info)),
               "table fills");
        expect(info.id == index + 1, "ids increase");
    }
    expect(manager.count() == FsHookManager::MaxHooks, "full count");
    expect(manager.install({0x2000 - ModuleBase, Replacement - ModuleBase, 0, 0},
                           ModuleBase, &info) == OutOfMemory,
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
